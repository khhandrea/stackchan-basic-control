#include "camera.h"

#include <cstring>

#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "img_converters.h"
#include "mbedtls/base64.h"
#include "rpc.h"

namespace camera {
namespace {

const char* TAG = "camera";
constexpr int kJpegQuality = 80;

bool ready = false;
SemaphoreHandle_t mutex = xSemaphoreCreateMutex();

struct Size {
    const char* name;
    framesize_t size;
} constexpr kSizes[] = {
    {"qqvga", FRAMESIZE_QQVGA},
    {"qvga", FRAMESIZE_QVGA},
    {"vga", FRAMESIZE_VGA},
};

// esp32-camera sizes its buffers at init, so a resolution change re-initializes the driver.
bool startCamera(framesize_t size) {
    camera_config_t config = {};
    config.pin_pwdn = -1;
    config.pin_reset = -1;
    config.pin_xclk = -1;      // GC0308 runs from its own crystal on CoreS3
    config.pin_sccb_sda = -1;  // reuse the internal I2C bus (port 1)
    config.pin_sccb_scl = -1;
    config.sccb_i2c_port = 1;
    config.pin_d0 = 39;
    config.pin_d1 = 40;
    config.pin_d2 = 41;
    config.pin_d3 = 42;
    config.pin_d4 = 15;
    config.pin_d5 = 16;
    config.pin_d6 = 48;
    config.pin_d7 = 47;
    config.pin_vsync = 46;
    config.pin_href = 38;
    config.pin_pclk = 45;
    config.xclk_freq_hz = 20000000;
    config.ledc_timer = LEDC_TIMER_0;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.pixel_format = PIXFORMAT_RGB565;
    config.frame_size = size;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;
    esp_err_t e = esp_camera_init(&config);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "camera init failed: %s", esp_err_to_name(e));
        return false;
    }
    // Let auto exposure settle
    for (int i = 0; i < 3; i++) {
        camera_fb_t* fb = esp_camera_fb_get();
        if (fb != nullptr) {
            esp_camera_fb_return(fb);
        }
    }
    return true;
}

cJSON* snapshot(const cJSON*, rpc::Error* err) {
    if (!ready) {
        return rpc::fail(err, rpc::kNotAvailable, "camera not available");
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    // Drop the frame captured before this request, so the snapshot is fresh
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb != nullptr) {
        esp_camera_fb_return(fb);
        fb = esp_camera_fb_get();
    }
    if (fb == nullptr) {
        xSemaphoreGive(mutex);
        return rpc::fail(err, rpc::kHardwareError, "capture failed");
    }
    uint8_t* jpeg = nullptr;
    size_t jpeg_len = 0;
    bool ok = frame2jpg(fb, kJpegQuality, &jpeg, &jpeg_len);
    int width = fb->width;
    int height = fb->height;
    esp_camera_fb_return(fb);
    xSemaphoreGive(mutex);
    if (!ok) {
        return rpc::fail(err, rpc::kInternalError, "jpeg encode failed");
    }

    size_t b64_len = 0;
    mbedtls_base64_encode(nullptr, 0, &b64_len, jpeg, jpeg_len);
    char* b64 = static_cast<char*>(heap_caps_malloc(b64_len + 1, MALLOC_CAP_SPIRAM));
    if (b64 == nullptr) {
        free(jpeg);
        return rpc::fail(err, rpc::kInternalError, "no memory");
    }
    mbedtls_base64_encode(reinterpret_cast<uint8_t*>(b64), b64_len + 1, &b64_len, jpeg, jpeg_len);
    b64[b64_len] = '\0';
    free(jpeg);

    cJSON* obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "width", width);
    cJSON_AddNumberToObject(obj, "height", height);
    cJSON_AddStringToObject(obj, "jpeg", b64);
    heap_caps_free(b64);
    return obj;
}

cJSON* setResolution(const cJSON* params, rpc::Error* err) {
    const char* name = rpc::getString(params, "size", err);
    if (name == nullptr) {
        return nullptr;
    }
    for (const Size& s : kSizes) {
        if (strcmp(s.name, name) == 0) {
            if (!ready) {
                return rpc::fail(err, rpc::kNotAvailable, "camera not available");
            }
            xSemaphoreTake(mutex, portMAX_DELAY);
            esp_camera_deinit();
            ready = startCamera(s.size);
            xSemaphoreGive(mutex);
            if (!ready) {
                return rpc::fail(err, rpc::kHardwareError, "camera restart failed");
            }
            return cJSON_CreateObject();
        }
    }
    return rpc::fail(err, rpc::kInvalidParams, "size: qqvga / qvga / vga");
}

const rpc::Method kMethods[] = {
    {"camera.snapshot", snapshot, nullptr},
    {"camera.set_resolution", setResolution, nullptr},
};

}  // namespace

void init() {
    if (!startCamera(FRAMESIZE_QVGA)) {
        events::error("sensor_init", "camera");
        return;
    }
    ready = true;
}

void registerMethods() {
    rpc::registerMethods(kMethods, sizeof(kMethods) / sizeof(kMethods[0]));
}

}  // namespace camera
