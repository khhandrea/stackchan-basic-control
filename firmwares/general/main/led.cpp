#include "led.h"

#include <atomic>
#include <cmath>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "poller.h"
#include "rpc.h"

namespace led {
namespace {

const char* TAG = "led";

// PY32L020 IO expander registers (StackChan-BSP PY32IOExpander)
constexpr uint8_t kVersion = 0x02;
constexpr uint8_t kGpioMode = 0x04;     // high byte: pins 8-13, 1 = output
constexpr uint8_t kGpioPullUp = 0x0A;   // high byte
constexpr uint8_t kGpioPullDown = 0x0C; // high byte
constexpr uint8_t kGpioDrive = 0x14;    // high byte, 1 = open drain
constexpr uint8_t kLedConfig = 0x24;    // bits5:0 count, bit6 refresh
constexpr uint8_t kLedRam = 0x30;       // RGB565 little-endian per LED
constexpr uint8_t kRgbPinBit = 1 << (13 - 8);

constexpr int kCount = 12;
constexpr int kMaxChannel = 168;  // safe limit from the official firmware

struct Rgb {
    uint8_t r = 0, g = 0, b = 0;
};

struct Fade {
    Rgb from;
    int64_t start_ms = 0;
    uint32_t duration_ms = 0;  // 0 = not fading
};

i2c::Device py32;
bool ready = false;
SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
Rgb target[kCount];  // requested colors
Rgb shown[kCount];   // current color before brightness
Fade fades[kCount];
int brightness = 100;
bool dirty = true;

// Effects run in tick(); any set_* call stops them
enum class Effect { None, Rainbow };
Effect effect = Effect::None;
uint32_t effect_period_ms = 3000;
int64_t effect_start_ms = 0;
int64_t effect_last_ms = 0;
constexpr int kEffectFrameMs = 40;

std::atomic<bool> power_save{false};
bool ps_active = false;  // tick's view (mutex held)
bool ps_led_on = false;
constexpr int kPowerSaveBlinkMs = 2000;
constexpr Rgb kPowerSaveColor = {51, 0, 0};  // red at 20 %, written as is

Rgb hsv(float h) {  // full saturation and value, h in degrees
    h = fmodf(h, 360.0f);
    float x = 1 - fabsf(fmodf(h / 60.0f, 2) - 1);
    float r = 0, g = 0, b = 0;
    if (h < 60) { r = 1; g = x; }
    else if (h < 120) { r = x; g = 1; }
    else if (h < 180) { g = 1; b = x; }
    else if (h < 240) { g = x; b = 1; }
    else if (h < 300) { r = x; b = 1; }
    else { r = 1; b = x; }
    return {static_cast<uint8_t>(r * 255), static_cast<uint8_t>(g * 255), static_cast<uint8_t>(b * 255)};
}

int64_t nowMs() {
    return esp_timer_get_time() / 1000;
}

uint8_t scale(uint8_t v) {
    int out = v * brightness / 100;
    return static_cast<uint8_t>(out > kMaxChannel ? kMaxChannel : out);
}

// Writes all LEDs (called with mutex held)
void flush() {
    uint8_t ram[kCount * 2];
    for (int i = 0; i < kCount; i++) {
        uint16_t c = static_cast<uint16_t>(((scale(shown[i].r) & 0xF8) << 8) | ((scale(shown[i].g) & 0xFC) << 3) |
                                           (scale(shown[i].b) >> 3));
        ram[i * 2] = static_cast<uint8_t>(c & 0xFF);
        ram[i * 2 + 1] = static_cast<uint8_t>(c >> 8);
    }
    if (py32.write(kLedRam, ram, sizeof(ram)) == ESP_OK) {
        py32.write8(kLedConfig, kCount | (1 << 6));
        dirty = false;
    }
}

// Power save frame: LED 0 only (called with mutex held)
void flushPowerSave(bool on) {
    uint8_t ram[kCount * 2] = {};
    if (on) {
        uint16_t c = static_cast<uint16_t>(((kPowerSaveColor.r & 0xF8) << 8) | ((kPowerSaveColor.g & 0xFC) << 3) |
                                           (kPowerSaveColor.b >> 3));
        ram[0] = static_cast<uint8_t>(c & 0xFF);
        ram[1] = static_cast<uint8_t>(c >> 8);
    }
    if (py32.write(kLedRam, ram, sizeof(ram)) == ESP_OK) {
        py32.write8(kLedConfig, kCount | (1 << 6));
    }
}

void setLed(int i, Rgb c, uint32_t fade_ms) {
    effect = Effect::None;
    if (fade_ms == 0) {
        shown[i] = c;
        fades[i].duration_ms = 0;
    } else {
        fades[i] = {shown[i], nowMs(), fade_ms};
    }
    target[i] = c;
    dirty = true;
}

void tick() {
    xSemaphoreTake(mutex, portMAX_DELAY);
    int64_t now = nowMs();
    if (power_save) {
        bool on = (now / kPowerSaveBlinkMs) % 2 == 0;
        if (!ps_active || on != ps_led_on) {
            flushPowerSave(on);
            ps_active = true;
            ps_led_on = on;
        }
        xSemaphoreGive(mutex);
        return;
    }
    if (ps_active) {
        ps_active = false;
        dirty = true;  // restore the normal colors
    }
    if (effect == Effect::Rainbow && now - effect_last_ms >= kEffectFrameMs) {
        // Hue gradient around the 12 LEDs, rotating once per period
        effect_last_ms = now;
        float phase = 360.0f * static_cast<float>((now - effect_start_ms) % effect_period_ms) / effect_period_ms;
        for (int i = 0; i < kCount; i++) {
            shown[i] = target[i] = hsv(phase + i * 360.0f / kCount);
            fades[i].duration_ms = 0;
        }
        dirty = true;
    }
    for (int i = 0; i < kCount; i++) {
        Fade& f = fades[i];
        if (f.duration_ms == 0) {
            continue;
        }
        int64_t t = now - f.start_ms;
        if (t >= f.duration_ms) {
            shown[i] = target[i];
            f.duration_ms = 0;
        } else {
            auto lerp = [&](uint8_t a, uint8_t b) {
                return static_cast<uint8_t>(a + (static_cast<int>(b) - a) * t / static_cast<int64_t>(f.duration_ms));
            };
            shown[i] = {lerp(f.from.r, target[i].r), lerp(f.from.g, target[i].g), lerp(f.from.b, target[i].b)};
        }
        dirty = true;
    }
    if (dirty) {
        flush();
    }
    xSemaphoreGive(mutex);
}

bool colorParam(const cJSON* params, Rgb* out, rpc::Error* err) {
    const cJSON* c = params ? cJSON_GetObjectItemCaseSensitive(params, "color") : nullptr;
    if (!cJSON_IsArray(c) || cJSON_GetArraySize(c) != 3) {
        rpc::fail(err, rpc::kInvalidParams, "color: [r, g, b] with 0-255");
        return false;
    }
    int v[3];
    for (int i = 0; i < 3; i++) {
        const cJSON* x = cJSON_GetArrayItem(c, i);
        if (!cJSON_IsNumber(x) || x->valuedouble < 0 || x->valuedouble > 255) {
            rpc::fail(err, rpc::kInvalidParams, "color: [r, g, b] with 0-255");
            return false;
        }
        v[i] = x->valueint;
    }
    *out = {static_cast<uint8_t>(v[0]), static_cast<uint8_t>(v[1]), static_cast<uint8_t>(v[2])};
    return true;
}

cJSON* notReady(rpc::Error* err) {
    return rpc::fail(err, rpc::kNotAvailable, "led controller not available");
}

cJSON* get(const cJSON*, rpc::Error* err) {
    if (!ready) return notReady(err);
    cJSON* obj = cJSON_CreateObject();
    cJSON* colors = cJSON_AddArrayToObject(obj, "colors");
    xSemaphoreTake(mutex, portMAX_DELAY);
    for (int i = 0; i < kCount; i++) {
        int rgb[3] = {target[i].r, target[i].g, target[i].b};
        cJSON_AddItemToArray(colors, cJSON_CreateIntArray(rgb, 3));
    }
    cJSON_AddNumberToObject(obj, "brightness", brightness);
    cJSON_AddStringToObject(obj, "effect", effect == Effect::Rainbow ? "rainbow" : "none");
    xSemaphoreGive(mutex);
    return obj;
}

cJSON* setAll(const cJSON* params, rpc::Error* err) {
    Rgb c;
    if (!ready) return notReady(err);
    if (!colorParam(params, &c, err)) return nullptr;
    xSemaphoreTake(mutex, portMAX_DELAY);
    for (int i = 0; i < kCount; i++) setLed(i, c, 0);
    xSemaphoreGive(mutex);
    return cJSON_CreateObject();
}

cJSON* setSide(const cJSON* params, rpc::Error* err) {
    Rgb c;
    int fade = 0;
    if (!ready) return notReady(err);
    const char* side = rpc::getString(params, "side", err);
    if (side == nullptr || !colorParam(params, &c, err) || !rpc::getOptInt(params, "fade_ms", 0, 60000, &fade, err)) {
        return nullptr;
    }
    int first;
    if (strcmp(side, "left") == 0) {
        first = 0;
    } else if (strcmp(side, "right") == 0) {
        first = 6;
    } else {
        return rpc::fail(err, rpc::kInvalidParams, "side: left / right");
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    for (int i = first; i < first + 6; i++) setLed(i, c, static_cast<uint32_t>(fade));
    xSemaphoreGive(mutex);
    return cJSON_CreateObject();
}

cJSON* setOne(const cJSON* params, rpc::Error* err) {
    Rgb c;
    int index = 0;
    if (!ready) return notReady(err);
    if (!rpc::getInt(params, "index", 0, kCount - 1, &index, err) || !colorParam(params, &c, err)) return nullptr;
    xSemaphoreTake(mutex, portMAX_DELAY);
    setLed(index, c, 0);
    xSemaphoreGive(mutex);
    return cJSON_CreateObject();
}

cJSON* setBrightness(const cJSON* params, rpc::Error* err) {
    int value = 0;
    if (!ready) return notReady(err);
    if (!rpc::getInt(params, "value", 0, 100, &value, err)) return nullptr;
    xSemaphoreTake(mutex, portMAX_DELAY);
    brightness = value;
    dirty = true;
    xSemaphoreGive(mutex);
    return cJSON_CreateObject();
}

cJSON* setEffect(const cJSON* params, rpc::Error* err) {
    int period = 3000;
    if (!ready) return notReady(err);
    const char* name = rpc::getString(params, "name", err);
    if (name == nullptr || !rpc::getOptInt(params, "period_ms", 500, 60000, &period, err)) return nullptr;
    Effect e;
    if (strcmp(name, "none") == 0) {
        e = Effect::None;
    } else if (strcmp(name, "rainbow") == 0) {
        e = Effect::Rainbow;
    } else {
        return rpc::fail(err, rpc::kInvalidParams, "name: none / rainbow");
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    effect = e;
    effect_period_ms = static_cast<uint32_t>(period);
    effect_start_ms = nowMs();
    effect_last_ms = 0;
    xSemaphoreGive(mutex);
    return cJSON_CreateObject();
}

const rpc::Method kMethods[] = {
    {"led.get", get, nullptr},
    {"led.set_effect", setEffect, nullptr},
    {"led.set_all", setAll, nullptr},
    {"led.set_side", setSide, nullptr},
    {"led.set_one", setOne, nullptr},
    {"led.set_brightness", setBrightness, nullptr},
};

}  // namespace

void init() {
    // The PY32 may boot slowly; wait up to ~1.2 s for a valid version register
    for (int i = 0; i < 6 && !ready; i++) {
        uint8_t version = 0;
        if (py32.isOpen() || py32.open(0x6F, 100000)) {
            ready = py32.read8(kVersion, &version) == ESP_OK && version != 0 && version != 0xFF;
        }
        if (!ready) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    if (!ready) {
        ESP_LOGE(TAG, "PY32 IO expander not found");
        events::error("sensor_init", "py32");
        return;
    }
    // RGB data pin (13): output, pull-up, push-pull — as in the official firmware
    py32.update8(kGpioMode, kRgbPinBit, kRgbPinBit);
    py32.update8(kGpioPullDown, kRgbPinBit, 0);
    py32.update8(kGpioPullUp, kRgbPinBit, kRgbPinBit);
    py32.update8(kGpioDrive, kRgbPinBit, 0);
    py32.write8(kLedConfig, kCount);
    vTaskDelay(pdMS_TO_TICKS(50));
    flush();
}

void registerMethods() {
    rpc::registerMethods(kMethods, sizeof(kMethods) / sizeof(kMethods[0]));
}

void setPowerSave(bool on) {
    power_save = on;
}

void start() {
    if (ready) {
        poller::add("led", tick, 20);
    }
}

}  // namespace led
