#include "idle.h"

#include <cstring>

#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "face.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "led.h"
#include "poller.h"
#include "rpc.h"
#include "servo.h"

namespace idle {
namespace {

const char* TAG = "idle";

enum Stage { Active, Sleepy, Dim, PowerSave };
constexpr const char* kStageNames[] = {"active", "sleepy", "dim", "power_save"};
constexpr int64_t kSleepyMs = 10 * 60 * 1000;
constexpr int64_t kDimMs = 30 * 60 * 1000;
constexpr int64_t kPowerSaveMs = 60 * 60 * 1000;
constexpr int64_t kStageStartMs[] = {0, kSleepyMs, kDimMs, kPowerSaveMs};

SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
int64_t last_ms = 0;  // last interaction (0 = boot)
Stage stage = Active;

int64_t nowMs() {
    return esp_timer_get_time() / 1000;
}

Stage stageFor(int64_t idle_ms) {
    if (idle_ms >= kPowerSaveMs) return PowerSave;
    if (idle_ms >= kDimMs) return Dim;
    if (idle_ms >= kSleepyMs) return Sleepy;
    return Active;
}

// Called with mutex held
void apply(Stage s) {
    if (s == stage) return;
    stage = s;
    face::setIdleSleepy(s >= Sleepy);
    display::setIdleDim(s >= Dim);
    display::setIdlePowerSave(s >= PowerSave);
    led::setPowerSave(s >= PowerSave);
    servo::setIdleOff(s >= Sleepy);
    ESP_LOGI(TAG, "stage %s", kStageNames[s]);
}

void tick() {
    xSemaphoreTake(mutex, portMAX_DELAY);
    apply(stageFor(nowMs() - last_ms));
    xSemaphoreGive(mutex);
}

bool passive(const char* m) {
    size_t n = strlen(m);
    if (n > 4 && strcmp(m + n - 4, ".get") == 0) return true;
    static const char* const kPassive[] = {
        "sys.ping",        "sys.info",          "sys.status",      "rpc.list",       "stream.start",
        "stream.stop",     "sys.subscribe",     "sys.unsubscribe", "display.capture", "camera.snapshot",
        "i2c.scan",        "i2c.read",          "log.set_level",   "log.set_power_interval",
        "idle.set_level",
    };
    for (const char* p : kPassive) {
        if (strcmp(p, m) == 0) return true;
    }
    return false;
}

void fillState(cJSON* obj) {
    int64_t now = nowMs();
    int64_t idle_ms = now - last_ms;
    cJSON_AddNumberToObject(obj, "idle_s", static_cast<double>(idle_ms / 1000));
    cJSON_AddNumberToObject(obj, "level", stage);
    cJSON_AddStringToObject(obj, "stage", kStageNames[stage]);
    if (stage < PowerSave) {
        cJSON_AddNumberToObject(obj, "next_in_s", static_cast<double>((kStageStartMs[stage + 1] - idle_ms) / 1000));
    } else {
        cJSON_AddNullToObject(obj, "next_in_s");
    }
}

cJSON* get(const cJSON*, rpc::Error*) {
    cJSON* obj = cJSON_CreateObject();
    xSemaphoreTake(mutex, portMAX_DELAY);
    fillState(obj);
    xSemaphoreGive(mutex);
    return obj;
}

// Jumps to a stage as if idle for exactly its start time; the normal timeline continues from there.
cJSON* setLevel(const cJSON* params, rpc::Error* err) {
    int level = 0;
    if (!rpc::getInt(params, "level", 0, 3, &level, err)) return nullptr;
    cJSON* obj = cJSON_CreateObject();
    xSemaphoreTake(mutex, portMAX_DELAY);
    last_ms = nowMs() - kStageStartMs[level];
    apply(static_cast<Stage>(level));
    fillState(obj);
    xSemaphoreGive(mutex);
    return obj;
}

const rpc::Method kMethods[] = {
    {"idle.get", get, nullptr},
    {"idle.set_level", setLevel, nullptr},
};

}  // namespace

void registerMethods() {
    rpc::registerMethods(kMethods, sizeof(kMethods) / sizeof(kMethods[0]));
}

void start() {
    poller::add("idle", tick, 500);
}

void activity() {
    xSemaphoreTake(mutex, portMAX_DELAY);
    last_ms = nowMs();
    apply(Active);
    xSemaphoreGive(mutex);
}

void onRequest(const char* method) {
    if (!passive(method)) activity();
}

void addStatus(cJSON* obj) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    int64_t idle_ms = nowMs() - last_ms;
    Stage s = stage;
    xSemaphoreGive(mutex);
    cJSON_AddNumberToObject(obj, "idle_s", static_cast<double>(idle_ms / 1000));
    cJSON_AddNumberToObject(obj, "idle_level", s);
}

}  // namespace idle
