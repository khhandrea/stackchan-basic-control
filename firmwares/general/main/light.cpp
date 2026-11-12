#include "light.h"

#include <atomic>
#include <cmath>

#include "esp_log.h"
#include "events.h"
#include "i2c_bus.h"
#include "poller.h"
#include "rpc.h"
#include "streams.h"

namespace light {
namespace {

const char* TAG = "light";

// LTR-553ALS registers
constexpr uint8_t kAlsContr = 0x80;     // bit0 active, bits4:2 gain
constexpr uint8_t kPsContr = 0x81;      // bits1:0 = 11 active
constexpr uint8_t kPsMeasRate = 0x84;
constexpr uint8_t kAlsMeasRate = 0x85;  // integration / repeat rate
constexpr uint8_t kPartId = 0x86;       // 0x92
constexpr uint8_t kAlsData = 0x88;      // CH1 L/H, CH0 L/H
constexpr uint8_t kPsData = 0x8D;       // 11-bit

// Proximity hysteresis (raw 0-2047)
constexpr int kNearThreshold = 400;
constexpr int kFarThreshold = 250;

i2c::Device ltr;
bool ready = false;
std::atomic<int> lux_x10{0};
std::atomic<int> proximity{0};
bool near = false;

// Lux from the LTR-553 application note (gain 1x, integration 100 ms)
float computeLux(int ch0, int ch1) {
    if (ch0 + ch1 == 0) {
        return 0;
    }
    float ratio = static_cast<float>(ch1) / (ch0 + ch1);
    float lux;
    if (ratio < 0.45f) {
        lux = 1.7743f * ch0 + 1.1059f * ch1;
    } else if (ratio < 0.64f) {
        lux = 4.2785f * ch0 - 1.9548f * ch1;
    } else if (ratio < 0.85f) {
        lux = 0.5926f * ch0 + 0.1185f * ch1;
    } else {
        lux = 0;
    }
    return lux < 0 ? 0 : lux;
}

void poll() {
    uint8_t als[4];
    uint8_t ps[2];
    if (ltr.read(kAlsData, als, 4) == ESP_OK) {
        int ch1 = als[0] | (als[1] << 8);
        int ch0 = als[2] | (als[3] << 8);
        lux_x10 = static_cast<int>(computeLux(ch0, ch1) * 10);
    }
    if (ltr.read(kPsData, ps, 2) == ESP_OK) {
        int value = ps[0] | ((ps[1] & 0x07) << 8);
        proximity = value;
        bool now_near = near ? value > kFarThreshold : value >= kNearThreshold;
        if (now_near != near) {
            near = now_near;
            cJSON* f = cJSON_CreateObject();
            cJSON_AddStringToObject(f, "state", near ? "near" : "far");
            cJSON_AddNumberToObject(f, "value", value);
            events::emit("proximity", f);
        }
    }
}

bool lightSource(cJSON* obj) {
    if (!ready) {
        return false;
    }
    cJSON_AddNumberToObject(obj, "lux", lux_x10 / 10.0);
    cJSON_AddNumberToObject(obj, "proximity", proximity.load());
    cJSON_AddBoolToObject(obj, "near", near);
    return true;
}

cJSON* get(const cJSON*, rpc::Error* err) {
    cJSON* obj = cJSON_CreateObject();
    if (!lightSource(obj)) {
        cJSON_Delete(obj);
        return rpc::fail(err, rpc::kNotAvailable, "light sensor not available");
    }
    return obj;
}

const rpc::Method kMethods[] = {
    {"light.get", get, nullptr},
};

}  // namespace

void init() {
    uint8_t id = 0;
    if (!ltr.open(0x23) || ltr.read8(kPartId, &id) != ESP_OK || id != 0x92) {
        ESP_LOGE(TAG, "LTR-553 not found (id 0x%02x)", id);
        events::error("sensor_init", "ltr553");
        return;
    }
    ltr.write8(kAlsMeasRate, 0x01);  // 100 ms integration, 100 ms repeat
    ltr.write8(kPsMeasRate, 0x00);   // 50 ms
    ltr.write8(kAlsContr, 0x01);     // active, gain 1x
    ltr.write8(kPsContr, 0x03);      // active
    ready = true;
}

void registerMethods() {
    rpc::registerMethods(kMethods, sizeof(kMethods) / sizeof(kMethods[0]));
}

void start() {
    if (!ready) {
        return;
    }
    poller::add("light", poll, 100);
    streams::add("light", lightSource);
}

}  // namespace light
