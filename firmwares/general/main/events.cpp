#include "events.h"

#include <atomic>
#include <cstring>

#include "notify.h"
#include "rpc.h"

namespace events {
namespace {

// Subscribable event names; bit i of the mask = kNames[i].
constexpr const char* kNames[] = {
    "touch.head", "touch.screen", "button",          "motion",       "proximity",
    "power.usb",  "power.charge", "power.low_battery", "servo.fault", "modal.result",
};
constexpr int kCount = sizeof(kNames) / sizeof(kNames[0]);

std::atomic<uint32_t> subscribed{0};

int indexOf(const char* name) {
    for (int i = 0; i < kCount; i++) {
        if (strcmp(kNames[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

// Name or group ("touch" = touch.*, "power" = power.*) → bit mask, 0 if unknown.
uint32_t maskOf(const char* name) {
    bool group = strcmp(name, "touch") == 0 || strcmp(name, "power") == 0;
    size_t len = strlen(name);
    uint32_t mask = 0;
    for (int i = 0; i < kCount; i++) {
        bool in_group = group && strncmp(kNames[i], name, len) == 0 && kNames[i][len] == '.';
        if (in_group || strcmp(kNames[i], name) == 0) {
            mask |= 1u << i;
        }
    }
    return mask;
}

cJSON* listJson() {
    cJSON* list = cJSON_CreateArray();
    uint32_t mask = subscribed.load();
    for (int i = 0; i < kCount; i++) {
        if (mask & (1u << i)) {
            cJSON_AddItemToArray(list, cJSON_CreateString(kNames[i]));
        }
    }
    return list;
}

// Parses params.events into a mask. Returns false (err set) on bad input.
bool parseEvents(const cJSON* params, uint32_t* mask, rpc::Error* err) {
    const cJSON* list = params ? cJSON_GetObjectItemCaseSensitive(params, "events") : nullptr;
    if (!cJSON_IsArray(list)) {
        rpc::fail(err, rpc::kInvalidParams, "events: list of names or groups");
        return false;
    }
    *mask = 0;
    const cJSON* it;
    cJSON_ArrayForEach(it, list) {
        uint32_t m = cJSON_IsString(it) ? maskOf(it->valuestring) : 0;
        if (m == 0) {
            rpc::fail(err, rpc::kInvalidParams, "events: unknown event name");
            return false;
        }
        *mask |= m;
    }
    return true;
}

cJSON* subscribe(const cJSON* params, rpc::Error* err) {
    uint32_t mask;
    if (!parseEvents(params, &mask, err)) {
        return nullptr;
    }
    subscribed |= mask;
    return listJson();
}

cJSON* unsubscribe(const cJSON* params, rpc::Error* err) {
    uint32_t mask;
    if (!parseEvents(params, &mask, err)) {
        return nullptr;
    }
    subscribed &= ~mask;
    return listJson();
}

const rpc::Method kMethods[] = {
    {"sys.subscribe", subscribe, nullptr},
    {"sys.unsubscribe", unsubscribe, nullptr},
};

}  // namespace

void registerMethods() {
    rpc::registerMethods(kMethods, sizeof(kMethods) / sizeof(kMethods[0]));
}

void emit(const char* name, cJSON* fields) {
    bool always = strcmp(name, "boot") == 0 || strcmp(name, "error") == 0;
    int i = indexOf(name);
    if (!always && (i < 0 || !(subscribed.load() & (1u << i)))) {
        cJSON_Delete(fields);
        return;
    }
    notify::event(name, fields);
}

void error(const char* kind, const char* detail) {
    cJSON* fields = cJSON_CreateObject();
    cJSON_AddStringToObject(fields, "kind", kind);
    cJSON_AddStringToObject(fields, "detail", detail);
    emit("error", fields);
}

}  // namespace events
