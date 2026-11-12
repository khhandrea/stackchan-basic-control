#include "display.h"

#include <atomic>
#include <cstring>

#include "esp_heap_caps.h"
#include "mbedtls/base64.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "face.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lcd.h"
#include "modal.h"
#include "power.h"
#include "rpc.h"

namespace display {
namespace {

constexpr int kFrameMs = 33;
constexpr int kDefaultBrightness = 128;  // 0-255

std::atomic<int> brightness{kDefaultBrightness};
std::atomic<bool> powered{true};
bool ready = false;

// display.capture: the display task copies the next frame into capture_buf
std::atomic<bool> capture_requested{false};
uint16_t* capture_buf = nullptr;
SemaphoreHandle_t capture_done = nullptr;

void applyBacklight() {
    int value = powered ? brightness.load() : 0;
    power::setBacklight(value == 0 ? 0 : (value * 100 + 254) / 255);
}

void renderFrame(int64_t now) {
    // Decide at the frame start, so a capture always holds one complete frame
    const bool capturing = capture_requested.load();
    face::update(now);
    modal::update(now);
    for (int y0 = 0; y0 < lcd::kHeight; y0 += lcd::kBandHeight) {
        gfx::Band band{lcd::acquireBand(), lcd::kWidth, y0, lcd::kBandHeight};
        face::render(band);
        modal::render(band);
        if (capturing) {
            memcpy(capture_buf + y0 * lcd::kWidth, band.px, lcd::kWidth * lcd::kBandHeight * sizeof(uint16_t));
        }
        lcd::pushBand(band.px, y0, lcd::kBandHeight);
    }
    if (capturing) {
        capture_requested = false;
        xSemaphoreGive(capture_done);
    }
}

// All LCD access happens on this task (esp_lcd panel IO is not shared between tasks safely);
// display.power only sets `powered`, applied here before the next frame.
void run(void*) {
    TickType_t last_wake = xTaskGetTickCount();
    bool panel_on = false;
    while (true) {
        int64_t now = esp_timer_get_time() / 1000;
        bool want_on = powered;
        if (want_on) {
            renderFrame(now);
            if (!panel_on) {
                // Panel and backlight on only after a full frame, so no stale content is shown
                lcd::setOn(true);
                applyBacklight();
                panel_on = true;
            }
        } else {
            if (panel_on) {
                applyBacklight();
                lcd::setOn(false);
                panel_on = false;
            }
            modal::update(now);  // keep modal timeouts running
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kFrameMs));
    }
}

cJSON* setBrightness(const cJSON* params, rpc::Error* err) {
    int value = 0;
    if (!rpc::getInt(params, "value", 0, 255, &value, err)) {
        return nullptr;
    }
    brightness = value;
    applyBacklight();
    return cJSON_CreateObject();
}

cJSON* setPower(const cJSON* params, rpc::Error* err) {
    bool on = true;
    if (!rpc::getBool(params, "on", &on, err)) {
        return nullptr;
    }
    if (!ready) {
        return rpc::fail(err, rpc::kNotAvailable, "display not available");
    }
    powered = on;
    return cJSON_CreateObject();
}

// Debug: returns the last rendered frame as base64 RGB565 (big-endian), 320x240.
cJSON* capture(const cJSON*, rpc::Error* err) {
    if (!ready || !powered) {
        return rpc::fail(err, rpc::kNotAvailable, "display off");
    }
    const size_t bytes = lcd::kWidth * lcd::kHeight * sizeof(uint16_t);
    if (capture_buf == nullptr) {
        capture_buf = static_cast<uint16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
        capture_done = xSemaphoreCreateBinary();
        if (capture_buf == nullptr) {
            return rpc::fail(err, rpc::kInternalError, "no memory");
        }
    }
    capture_requested = true;
    if (xSemaphoreTake(capture_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        capture_requested = false;
        return rpc::fail(err, rpc::kBusy, "capture timeout");
    }
    size_t out_len = 0;
    mbedtls_base64_encode(nullptr, 0, &out_len, reinterpret_cast<const uint8_t*>(capture_buf), bytes);
    char* out = static_cast<char*>(heap_caps_malloc(out_len + 1, MALLOC_CAP_SPIRAM));
    if (out == nullptr) {
        return rpc::fail(err, rpc::kInternalError, "no memory");
    }
    mbedtls_base64_encode(reinterpret_cast<uint8_t*>(out), out_len + 1, &out_len,
                          reinterpret_cast<const uint8_t*>(capture_buf), bytes);
    out[out_len] = '\0';
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "width", lcd::kWidth);
    cJSON_AddNumberToObject(obj, "height", lcd::kHeight);
    cJSON_AddStringToObject(obj, "format", "rgb565be");
    cJSON_AddStringToObject(obj, "data", out);
    heap_caps_free(out);
    return obj;
}

const rpc::Method kMethods[] = {
    {"display.capture", capture, nullptr},
    {"display.set_brightness", setBrightness, nullptr},
    {"display.power", setPower, nullptr},
};

}  // namespace

bool init() {
    ready = lcd::init();
    return ready;
}

void registerMethods() {
    face::registerMethods();
    modal::registerMethods();
    rpc::registerMethods(kMethods, sizeof(kMethods) / sizeof(kMethods[0]));
}

void start() {
    if (ready) {
        xTaskCreatePinnedToCore(run, "display", 6144, nullptr, 3, nullptr, 0);
    }
}

}  // namespace display
