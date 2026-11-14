#include "imu.h"

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
#include "streams.h"

extern "C" {
extern const uint8_t bmi270_config_file[];
extern const size_t bmi270_config_file_size;
}

namespace imu {
namespace {

const char* TAG = "imu";

// BMI270 registers
constexpr uint8_t kChipId = 0x00;
constexpr uint8_t kStatus = 0x03;  // bit2 aux_busy
constexpr uint8_t kAuxData = 0x04; // aux (8) + acc (6) + gyr (6)
constexpr uint8_t kInternalStatus = 0x21;
constexpr uint8_t kTemperature = 0x22;  // int16, 0 = 23 C, 1/512 K per LSB, 0x8000 = invalid
constexpr uint8_t kAuxDevId = 0x4B;
constexpr uint8_t kAuxIfConf = 0x4C;
constexpr uint8_t kAuxRdAddr = 0x4D;
constexpr uint8_t kAuxWrAddr = 0x4E;
constexpr uint8_t kAuxWrData = 0x4F;
constexpr uint8_t kIntMapData = 0x58;
constexpr uint8_t kInitCtrl = 0x59;
constexpr uint8_t kInitAddr0 = 0x5B;
constexpr uint8_t kInitData = 0x5E;
constexpr uint8_t kIfConf = 0x6B;
constexpr uint8_t kPwrConf = 0x7C;
constexpr uint8_t kPwrCtrl = 0x7D;
constexpr uint8_t kCmd = 0x7E;

// Default ranges after the config upload: accel +-8 g, gyro +-2000 dps
constexpr float kAccelScale = 8.0f / 32768.0f;
constexpr float kGyroScale = 2000.0f / 32768.0f;
// BMM150 without trim compensation (approximate): X/Y 13-bit ~0.3 uT, Z 15-bit ~0.15 uT
constexpr float kMagXyScale = 0.3f;
constexpr float kMagZScale = 0.15f;

constexpr int kPollMs = 20;

i2c::Device bmi;
bool ready = false;
bool has_mag = false;
SemaphoreHandle_t mutex = xSemaphoreCreateMutex();

struct Sample {
    float a[3] = {0, 0, 1};
    float g[3] = {};
    float m[3] = {};
    float tilt_deg = 0;
    float temp_c = NAN;
    const char* motion = "still";
} sample;  // guarded by mutex

// ---- motion detection state (poller task only) ----
float ref[3] = {0, 0, 1};  // "up" direction (unit), measured at start
float lp[3] = {0, 0, 1};   // low-passed accel
enum class Tilt { Upright, Tilted, Fallen } tilt_state = Tilt::Upright;
int64_t tilt_since = 0;
bool tilted_sent = false, fallen_sent = false;
int64_t shake_times[4] = {};
int shake_index = 0;
int64_t shake_cooldown_until = 0;
int64_t nudge_times[3] = {};
int nudge_index = 0;
int64_t nudge_cooldown_until = 0;
int temp_div = 0;
float temp_c = NAN;
int lift_count = 0;
int64_t lift_cooldown_until = 0;

void delayMs(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms) + 1);
}

uint8_t reg8(uint8_t reg) {
    uint8_t v = 0;
    bmi.read8(reg, &v);
    return v;
}

void auxWaitIdle() {
    for (int i = 0; i < 5 && (reg8(kStatus) & 0x04); i++) {
        delayMs(1);
    }
}

void auxWrite(uint8_t reg, uint8_t value) {
    bmi.write8(kAuxWrData, value);
    bmi.write8(kAuxWrAddr, reg);
    auxWaitIdle();
}

uint8_t auxRead(uint8_t reg) {
    bmi.write8(kAuxIfConf, 0x80);  // manual mode, burst 1
    bmi.write8(kAuxRdAddr, reg);
    auxWaitIdle();
    return reg8(kAuxData);
}

bool uploadConfig() {
    constexpr size_t kChunk = 256;
    for (size_t i = 0; i < bmi270_config_file_size; i += kChunk) {
        const uint8_t addr[2] = {static_cast<uint8_t>((i / 2) & 0x0F), static_cast<uint8_t>((i / 2) >> 4)};
        size_t n = bmi270_config_file_size - i < kChunk ? bmi270_config_file_size - i : kChunk;
        if (bmi.write(kInitAddr0, addr, 2) != ESP_OK || bmi.write(kInitData, bmi270_config_file + i, n) != ESP_OK) {
            return false;
        }
    }
    return true;
}

// Sequence after M5Unified BMI270_Class::begin (MIT): reset, config upload, BMM150 on the aux bus.
bool initBmi270() {
    if (reg8(kChipId) != 0x24) {
        return false;
    }
    bmi.write8(kCmd, 0xB6);  // soft reset
    delayMs(2);
    for (int i = 0; i < 16 && reg8(kPwrConf) == 0; i++) {
        delayMs(1);
    }
    bmi.write8(kPwrConf, 0x00);  // advanced power save off
    delayMs(1);
    bmi.write8(kInitCtrl, 0x00);
    bool uploaded = uploadConfig();
    bmi.write8(kInitCtrl, 0x01);
    bmi.write8(kIntMapData, 0xFF);
    if (!uploaded) {
        return false;
    }
    bool initialized = false;
    for (int i = 0; i < 30 && !initialized; i++) {
        delayMs(2);
        initialized = (reg8(kInternalStatus) & 0x0F) == 0x01;
    }
    if (!initialized) {
        ESP_LOGE(TAG, "BMI270 init status 0x%02x", reg8(kInternalStatus));
        return false;
    }

    // BMM150 at 0x10 behind the aux interface
    bmi.write8(kIfConf, 0x20);    // aux I2C on
    bmi.write8(kPwrConf, 0x00);
    bmi.write8(kPwrCtrl, 0x0E);   // temp, gyro, accel (aux off while configuring)
    bmi.write8(kAuxIfConf, 0x80); // manual mode
    bmi.write8(kAuxDevId, 0x10 << 1);
    auxWrite(0x4B, 0x83);         // BMM150 soft reset + power on
    auxRead(0x40);
    has_mag = auxRead(0x40) == 0x32;
    if (has_mag) {
        auxWrite(0x4C, 0x38);           // normal mode, 30 Hz
        bmi.write8(kAuxIfConf, 0x4F);   // auto mode, burst 8
        bmi.write8(kAuxRdAddr, 0x42);   // BMM150 data X LSB
        bmi.write8(kPwrCtrl, 0x0F);     // + aux
    } else {
        ESP_LOGW(TAG, "BMM150 not found");
    }
    return true;
}

bool readRaw(float a[3], float g[3], float m[3]) {
    uint8_t buf[20];
    if (bmi.read(kAuxData, buf, sizeof(buf)) != ESP_OK) {
        return false;
    }
    int16_t v[10];
    memcpy(v, buf, sizeof(buf));  // little-endian int16
    for (int i = 0; i < 3; i++) {
        a[i] = v[4 + i] * kAccelScale;
        g[i] = v[7 + i] * kGyroScale;
    }
    m[0] = (v[0] >> 3) * kMagXyScale;
    m[1] = (v[1] >> 3) * kMagXyScale;
    m[2] = (v[2] >> 1) * kMagZScale;
    return true;
}

float norm(const float v[3]) {
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

std::atomic<Motion> motion_state{Motion::Still};
std::atomic<uint32_t> shake_count{0};
std::atomic<uint32_t> nudge_count{0};

void emitMotion(const char* kind) {
    cJSON* f = cJSON_CreateObject();
    cJSON_AddStringToObject(f, "kind", kind);
    events::emit("motion", f);
}

void calibrateUp() {
    // Wait for the first accelerometer sample (STATUS bit7 drdy_acc) after power-up
    for (int i = 0; i < 50 && !(reg8(kStatus) & 0x80); i++) {
        delayMs(2);
    }
    float sum[3] = {};
    int n = 0;
    for (int i = 0; i < 10; i++) {
        float a[3], g[3], m[3];
        if (readRaw(a, g, m)) {
            for (int k = 0; k < 3; k++) sum[k] += a[k];
            n++;
        }
        delayMs(10);
    }
    float len = norm(sum) / (n ? n : 1);
    if (n > 0 && len > 0.8f && len < 1.2f) {
        float l = norm(sum);
        for (int k = 0; k < 3; k++) {
            ref[k] = sum[k] / l;
            lp[k] = sum[k] / n;
        }
    }
    ESP_LOGI(TAG, "up = (%.2f, %.2f, %.2f)", ref[0], ref[1], ref[2]);
}

void poll() {
    float a[3], g[3], m[3];
    if (!readRaw(a, g, m)) {
        return;
    }
    int64_t now = esp_timer_get_time() / 1000;
    for (int k = 0; k < 3; k++) {
        lp[k] += 0.1f * (a[k] - lp[k]);
    }

    // Tilt from the low-passed gravity direction
    float l = norm(lp);
    float cosang = l > 0.01f ? (lp[0] * ref[0] + lp[1] * ref[1] + lp[2] * ref[2]) / l : 1;
    cosang = cosang > 1 ? 1 : (cosang < -1 ? -1 : cosang);
    float tilt = acosf(cosang) * 180.0f / 3.14159265f;
    Tilt t = tilt < 25 ? Tilt::Upright : (tilt < 60 ? Tilt::Tilted : Tilt::Fallen);
    if (t != tilt_state) {
        tilt_state = t;
        tilt_since = now;
        if (t == Tilt::Upright) {
            tilted_sent = fallen_sent = false;
        }
    }
    if (t == Tilt::Tilted && !tilted_sent && now - tilt_since >= 300) {
        tilted_sent = true;
        emitMotion("tilted");
    }
    if (t == Tilt::Fallen && !fallen_sent && now - tilt_since >= 1000) {
        fallen_sent = true;
        emitMotion("fallen");
    }

    // Shake: 4 jolts (> 0.7 g away from the low-passed value) within 800 ms
    float jolt[3] = {a[0] - lp[0], a[1] - lp[1], a[2] - lp[2]};
    bool shaking = false;
    if (norm(jolt) > 0.7f) {
        shake_times[shake_index] = now;
        shake_index = (shake_index + 1) % 4;
        int64_t oldest = shake_times[shake_index];
        if (oldest != 0 && now - oldest <= 800 && now >= shake_cooldown_until) {
            shake_cooldown_until = now + 1500;
            shake_count++;
            emitMotion("shaken");
        }
        shaking = true;
    }

    // Light shake ("nudged"): 3 jolts > 0.3 g within 800 ms, but not a full shake
    if (norm(jolt) > 0.3f) {
        nudge_times[nudge_index] = now;
        nudge_index = (nudge_index + 1) % 3;
        int64_t oldest = nudge_times[nudge_index];
        if (oldest != 0 && now - oldest <= 800 && now >= nudge_cooldown_until && now >= shake_cooldown_until) {
            nudge_cooldown_until = now + 1500;
            nudge_count++;
            emitMotion("nudged");
        }
    }

    // Board temperature from the BMI270, once a second
    if (++temp_div >= 1000 / kPollMs) {
        temp_div = 0;
        uint8_t tb[2];
        if (bmi.read(kTemperature, tb, 2) == ESP_OK) {
            int16_t raw = static_cast<int16_t>(tb[0] | (tb[1] << 8));
            temp_c = raw == INT16_MIN ? NAN : 23.0f + raw / 512.0f;
        }
    }

    // Lift: upward acceleration along "up" for >= 60 ms while upright
    float up_accel = a[0] * ref[0] + a[1] * ref[1] + a[2] * ref[2] - 1.0f;
    if (t == Tilt::Upright && !shaking && up_accel > 0.12f) {
        if (++lift_count >= 3 && now >= lift_cooldown_until) {
            lift_cooldown_until = now + 2000;
            emitMotion("lifted");
        }
    } else {
        lift_count = 0;
    }

    float jolt_n = norm(jolt);
    float gyro_n = norm(g);
    const char* motion = t == Tilt::Fallen ? "fallen"
                         : t == Tilt::Tilted ? "tilted"
                         : (gyro_n > 20 || jolt_n > 0.1f) ? "moving"
                                                          : "still";
    xSemaphoreTake(mutex, portMAX_DELAY);
    memcpy(sample.a, a, sizeof(a));
    memcpy(sample.g, g, sizeof(g));
    memcpy(sample.m, m, sizeof(m));
    sample.tilt_deg = tilt;
    sample.temp_c = temp_c;
    sample.motion = motion;
    xSemaphoreGive(mutex);
    motion_state = t == Tilt::Fallen ? Motion::Fallen
                   : t == Tilt::Tilted ? Motion::Tilted
                   : (gyro_n > 20 || jolt_n > 0.1f) ? Motion::Moving
                                                    : Motion::Still;
}

double round2(float v) {
    return roundf(v * 100) / 100.0;
}

bool imuSource(cJSON* obj) {
    if (!ready) {
        return false;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    Sample s = sample;
    xSemaphoreGive(mutex);
    const char* an[3] = {"ax", "ay", "az"};
    const char* gn[3] = {"gx", "gy", "gz"};
    const char* mn[3] = {"mx", "my", "mz"};
    for (int k = 0; k < 3; k++) {
        cJSON_AddNumberToObject(obj, an[k], roundf(s.a[k] * 1000) / 1000.0);
    }
    for (int k = 0; k < 3; k++) {
        cJSON_AddNumberToObject(obj, gn[k], round2(s.g[k]));
    }
    for (int k = 0; k < 3; k++) {
        if (has_mag) {
            cJSON_AddNumberToObject(obj, mn[k], round2(s.m[k]));
        } else {
            cJSON_AddNullToObject(obj, mn[k]);
        }
    }
    cJSON_AddNumberToObject(obj, "tilt_deg", round2(s.tilt_deg));
    cJSON_AddStringToObject(obj, "motion", s.motion);
    if (std::isnan(s.temp_c)) {
        cJSON_AddNullToObject(obj, "temp_c");
    } else {
        cJSON_AddNumberToObject(obj, "temp_c", roundf(s.temp_c * 10) / 10.0);
    }
    return true;
}

cJSON* get(const cJSON*, rpc::Error* err) {
    cJSON* obj = cJSON_CreateObject();
    if (!imuSource(obj)) {
        cJSON_Delete(obj);
        return rpc::fail(err, rpc::kNotAvailable, "imu not available");
    }
    return obj;
}

const rpc::Method kMethods[] = {
    {"imu.get", get, nullptr},
};

}  // namespace

void init() {
    if (!bmi.open(0x69) || !initBmi270()) {
        ESP_LOGE(TAG, "BMI270 init failed");
        events::error("sensor_init", "bmi270");
        return;
    }
    ready = true;
    calibrateUp();
}

void registerMethods() {
    rpc::registerMethods(kMethods, sizeof(kMethods) / sizeof(kMethods[0]));
}

void start() {
    if (!ready) {
        return;
    }
    poller::add("imu", poll, kPollMs);
    streams::add("imu", imuSource);
}

Motion motion() {
    return motion_state;
}

uint32_t shakeCount() {
    return shake_count;
}

uint32_t nudgeCount() {
    return nudge_count;
}

}  // namespace imu
