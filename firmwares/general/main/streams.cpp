#include "streams.h"

#include <cstring>
#include <vector>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "notify.h"
#include "poller.h"
#include "rpc.h"

namespace streams {
namespace {

struct Stream {
    const char* name;
    Source source;
    uint32_t period_ms = 0;  // 0 = stopped
    int64_t next_ms = 0;
    uint32_t seq = 0;
};

std::vector<Stream> list;
SemaphoreHandle_t mutex = nullptr;

Stream* find(const char* name) {
    for (Stream& s : list) {
        if (strcmp(s.name, name) == 0) {
            return &s;
        }
    }
    return nullptr;
}

void send(Stream& s, int64_t now) {
    cJSON* params = cJSON_CreateObject();
    cJSON_AddNumberToObject(params, "seq", s.seq++);
    cJSON_AddNumberToObject(params, "ts_ms", static_cast<double>(now));
    cJSON_AddStringToObject(params, "name", s.name);
    if (!s.source(params)) {
        cJSON_Delete(params);
        return;
    }
    cJSON* msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "method", "notify.stream");
    cJSON_AddItemToObject(msg, "params", params);
    notify::sendMessage(msg);
}

void tick() {
    int64_t now = esp_timer_get_time() / 1000;
    xSemaphoreTake(mutex, portMAX_DELAY);
    for (Stream& s : list) {
        if (s.period_ms != 0 && now >= s.next_ms) {
            s.next_ms += s.period_ms;
            if (s.next_ms <= now) {
                s.next_ms = now + s.period_ms;
            }
            send(s, now);
        }
    }
    xSemaphoreGive(mutex);
}

Stream* streamParam(const cJSON* params, rpc::Error* err) {
    const char* name = rpc::getString(params, "name", err);
    if (name == nullptr) {
        return nullptr;
    }
    Stream* s = find(name);
    if (s == nullptr) {
        rpc::fail(err, rpc::kInvalidParams, "name: unknown stream");
    }
    return s;
}

cJSON* startStream(const cJSON* params, rpc::Error* err) {
    int rate = 0;
    Stream* s = streamParam(params, err);
    if (s == nullptr || !rpc::getInt(params, "rate_hz", 1, 100, &rate, err)) {
        return nullptr;
    }
    cJSON* probe = cJSON_CreateObject();
    bool available = s->source(probe);
    cJSON_Delete(probe);
    if (!available) {
        return rpc::fail(err, rpc::kNotAvailable, "stream source not available");
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    s->period_ms = 1000 / static_cast<uint32_t>(rate);
    s->next_ms = esp_timer_get_time() / 1000;
    xSemaphoreGive(mutex);
    return cJSON_CreateObject();
}

cJSON* stopStream(const cJSON* params, rpc::Error* err) {
    Stream* s = streamParam(params, err);
    if (s == nullptr) {
        return nullptr;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    s->period_ms = 0;
    xSemaphoreGive(mutex);
    return cJSON_CreateObject();
}

const rpc::Method kMethods[] = {
    {"stream.start", startStream, nullptr},
    {"stream.stop", stopStream, nullptr},
};

}  // namespace

void add(const char* name, Source source) {
    list.push_back({name, source});
}

void registerMethods() {
    rpc::registerMethods(kMethods, sizeof(kMethods) / sizeof(kMethods[0]));
}

void start() {
    mutex = xSemaphoreCreateMutex();
    poller::add("streams", tick, 10);
}

}  // namespace streams
