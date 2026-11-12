#include "rpc.h"

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
