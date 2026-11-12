#include "rpc.h"

#include <cstdio>
#include <cstring>

#include "link.h"
#include "notify.h"

namespace rpc {
namespace {

std::vector<Method> registry;

const Method* findMethod(const char* name) {
    for (const Method& m : registry) {
        if (strcmp(m.name, name) == 0) {
            return &m;
        }
    }
    return nullptr;
}

cJSON* responseWithId(const cJSON* id) {
    cJSON* msg = cJSON_CreateObject();
    cJSON_AddItemToObject(msg, "id", id != nullptr ? cJSON_Duplicate(id, false) : cJSON_CreateNull());
    return msg;
}

bool validId(const cJSON* id) {
    return cJSON_IsNumber(id) || cJSON_IsString(id);
}

}  // namespace

void registerMethods(const Method* methods, size_t count) {
    registry.insert(registry.end(), methods, methods + count);
}

const std::vector<Method>& methods() {
    return registry;
}

void sendError(const cJSON* id, int code, const char* message) {
    cJSON* msg = responseWithId(id);
    cJSON* error = cJSON_AddObjectToObject(msg, "error");
    cJSON_AddNumberToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    notify::sendMessage(msg);
}

void handleLine(const char* line) {
    cJSON* req = cJSON_Parse(line);
    if (req == nullptr) {
        sendError(nullptr, kParseError, "parse error");
        return;
    }

    const cJSON* id = cJSON_GetObjectItemCaseSensitive(req, "id");
    const cJSON* method = cJSON_GetObjectItemCaseSensitive(req, "method");
    const cJSON* params = cJSON_GetObjectItemCaseSensitive(req, "params");

    if (!cJSON_IsObject(req) || (id != nullptr && !validId(id))) {
        sendError(nullptr, kInvalidRequest, "invalid request");
    } else if (!cJSON_IsString(method)) {
        sendError(id, kInvalidRequest, "missing method");
    } else if (params != nullptr && !cJSON_IsObject(params)) {
        sendError(id, kInvalidParams, "params must be an object");
    } else if (const Method* m = findMethod(method->valuestring); m == nullptr) {
        if (id != nullptr) {
            sendError(id, kMethodNotFound, "method not found");
        }
    } else {
        Error err;
        cJSON* result = m->handler(params, &err);
        if (id != nullptr) {
            if (result != nullptr) {
                cJSON* msg = responseWithId(id);
                cJSON_AddItemToObject(msg, "result", result);
                notify::sendMessage(msg);
            } else {
                sendError(id, err.code != 0 ? err.code : kInternalError,
                          err.message != nullptr ? err.message : "internal error");
            }
        } else {
            cJSON_Delete(result);
        }
        if (result != nullptr && m->after_response != nullptr) {
            m->after_response();
        }
    }

    cJSON_Delete(req);
}

cJSON* fail(Error* err, int code, const char* message) {
    err->code = code;
    err->message = message;
    return nullptr;
}

namespace {

// Messages must outlive the handler; built into a static buffer (handlers run on one task).
char param_message[96];

bool paramError(Error* err, const char* name, const char* what) {
    snprintf(param_message, sizeof(param_message), "%s: %s", name, what);
    fail(err, kInvalidParams, param_message);
    return false;
}

const cJSON* item(const cJSON* params, const char* name) {
    return params != nullptr ? cJSON_GetObjectItemCaseSensitive(params, name) : nullptr;
}

}  // namespace

bool getOptNumber(const cJSON* params, const char* name, double min, double max, double* out, Error* err) {
    const cJSON* it = item(params, name);
    if (it == nullptr) {
        return true;
    }
    if (!cJSON_IsNumber(it)) {
        return paramError(err, name, "must be a number");
    }
    if (it->valuedouble < min || it->valuedouble > max) {
        snprintf(param_message, sizeof(param_message), "%s out of range %g-%g", name, min, max);
        fail(err, kInvalidParams, param_message);
        return false;
    }
    *out = it->valuedouble;
    return true;
}

bool getNumber(const cJSON* params, const char* name, double min, double max, double* out, Error* err) {
    if (item(params, name) == nullptr) {
        return paramError(err, name, "missing");
    }
    return getOptNumber(params, name, min, max, out, err);
}

bool getOptInt(const cJSON* params, const char* name, int min, int max, int* out, Error* err) {
    double value = *out;
    if (!getOptNumber(params, name, min, max, &value, err)) {
        return false;
    }
    if (value != static_cast<int>(value)) {
        return paramError(err, name, "must be an integer");
    }
    *out = static_cast<int>(value);
    return true;
}

bool getInt(const cJSON* params, const char* name, int min, int max, int* out, Error* err) {
    if (item(params, name) == nullptr) {
        return paramError(err, name, "missing");
    }
    return getOptInt(params, name, min, max, out, err);
}

bool getOptBool(const cJSON* params, const char* name, bool* out, Error* err) {
    const cJSON* it = item(params, name);
    if (it == nullptr) {
        return true;
    }
    if (!cJSON_IsBool(it)) {
        return paramError(err, name, "must be a boolean");
    }
    *out = cJSON_IsTrue(it);
    return true;
}

bool getBool(const cJSON* params, const char* name, bool* out, Error* err) {
    if (item(params, name) == nullptr) {
        return paramError(err, name, "missing");
    }
    return getOptBool(params, name, out, err);
}

const char* getOptString(const cJSON* params, const char* name, Error* err, bool* ok) {
    const cJSON* it = item(params, name);
    *ok = true;
    if (it == nullptr) {
        return nullptr;
    }
    if (!cJSON_IsString(it)) {
        *ok = paramError(err, name, "must be a string");
        return nullptr;
    }
    return it->valuestring;
}

const char* getString(const cJSON* params, const char* name, Error* err) {
    bool ok;
    const char* s = getOptString(params, name, err, &ok);
    if (ok && s == nullptr) {
        paramError(err, name, "missing");
    }
    return s;
}

void serve() {
    static char line[hostlink::kMaxRequestLine + 1];
    while (true) {
        switch (hostlink::readLine(line, sizeof(line))) {
            case hostlink::ReadResult::TooLong:
                sendError(nullptr, kInvalidRequest, "line too long");
                break;
            case hostlink::ReadResult::Line:
                if (line[0] != '\0') {
                    handleLine(line);
                }
                break;
        }
    }
}

}  // namespace rpc
