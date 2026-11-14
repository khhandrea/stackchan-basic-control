#include "servo.h"

#include <atomic>
#include <cmath>
#include <cstring>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "poller.h"
#include "rpc.h"
#include "streams.h"

namespace servo {
namespace {

const char* TAG = "servo";

constexpr uart_port_t kUart = UART_NUM_1;
constexpr int kTxPin = 6, kRxPin = 7;
constexpr int kBaud = 1000000;

// SCSCL registers (big-endian words)
constexpr uint8_t kTorqueEnable = 40;
constexpr uint8_t kGoalPosition = 42;  // position, time, speed
constexpr uint8_t kPresentPosition = 56;  // position, speed, load (words), voltage, temperature (bytes)
constexpr uint8_t kInstRead = 0x02, kInstWrite = 0x03;

constexpr float kRawPerDeg = 3.2f;  // 1 raw step = 0.3125 deg (official firmware)
constexpr int kStepMs = 20;
constexpr float kMinSpeed = 10;          // deg/s, lowest accepted speed
constexpr float kMaxSpeed = 180;         // deg/s, upper limit for every move
constexpr float kDefaultMinSpeed = 20;   // deg/s, servo.move / home: start and end speed
constexpr float kDefaultMaxSpeed = 90;   // deg/s, servo.move / home: cruise speed
constexpr float kGentleSpeed = 20;       // deg/s, easing back inside the limits and the reaction lift
constexpr float kAccel = 120;            // deg/s^2, from the start speed up to the cruise speed and back

constexpr int kOverheatC = 65;
constexpr int kOverloadLoad = 700;   // of 1023
constexpr int kOverloadSamples = 5;  // x 100 ms
constexpr float kStallErrorDeg = 5;
constexpr int kStallMs = 1000;
constexpr int kBootTimeoutMs = 2000;  // servo answer after VM_EN on
constexpr int kVmOffMinMs = 1000;     // VM_EN off time before power on, so the servos fully reset
constexpr int kStableReads = 3;       // consecutive matching readings before trusting the position
constexpr float kStableDeg = 1.0f;
constexpr float kPowerOnMarginDeg = 15;  // refuse power on if a reading is this far outside the limits

struct Axis {
    const char* name;
    uint8_t id;
    int zero;
    float min_deg, max_deg;
    // motion (guarded by mutex)
    float command = 0;  // last commanded angle
    float vel = 0;      // deg/s of the commanded angle (signed)
    float target = 0;
    float speed = kGentleSpeed;      // cruise
    float min_speed = kGentleSpeed;  // start and end
    bool torque = false;
    // feedback
    int overload_count = 0;
    int64_t stall_since = 0;
};

Axis axes[2] = {
    {"yaw", 1, 460, -128, 128},
    {"pitch", 2, 620, 5, 85},
};

i2c::Device py32;
bool ready = false;
std::atomic<bool> powered{false};
int64_t vm_off_ms = 0;  // when VM_EN was last turned off (0 = unknown, treated as long ago)
// reactLift state (guarded by mutex)
constexpr float kLiftDeg = 8;
bool lifted = false;
float lift_base = 0, lift_to = 0;
SemaphoreHandle_t bus_mutex = nullptr;  // UART transactions
SemaphoreHandle_t mutex = nullptr;      // axis state
SemaphoreHandle_t power_mutex = nullptr;  // power on / off sequences (RPC and the auto task)
// Auto power: on at boot and whenever wanted; off while idle (sleepy stage and deeper).
// servo.power and faults change "wanted"; a failed automatic power-on stops retrying until servo.power on.
std::atomic<bool> wanted{true};
std::atomic<bool> idle_off{false};
std::atomic<bool> auto_failed{false};
constexpr int kAutoBootDelayMs = 2000;
constexpr int kAutoPeriodMs = 200;

// ---- SCS protocol ----

bool transact(uint8_t id, uint8_t inst, const uint8_t* params, int n, uint8_t* resp, int resp_len) {
    uint8_t pkt[32];
    int len = n + 2;
    pkt[0] = pkt[1] = 0xFF;
    pkt[2] = id;
    pkt[3] = static_cast<uint8_t>(len);
    pkt[4] = inst;
    uint8_t sum = id + len + inst;
    for (int i = 0; i < n; i++) {
        pkt[5 + i] = params[i];
        sum += params[i];
    }
    pkt[5 + n] = static_cast<uint8_t>(~sum);
    const int pkt_len = 6 + n;

    xSemaphoreTake(bus_mutex, portMAX_DELAY);
    uart_flush_input(kUart);
    uart_write_bytes(kUart, pkt, pkt_len);
    uart_wait_tx_done(kUart, pdMS_TO_TICKS(10));

    // Response: FF FF ID LEN ERR PARAMS CHK. The half-duplex line may echo our own packet first.
    uint8_t buf[64];
    int got = 0;
    int64_t deadline = esp_timer_get_time() + 15000;
    bool ok = false;
    while (esp_timer_get_time() < deadline && got < static_cast<int>(sizeof(buf))) {
        int r = uart_read_bytes(kUart, buf + got, sizeof(buf) - got, pdMS_TO_TICKS(2));
        if (r > 0) got += r;
        int start = 0;
        if (got >= pkt_len && memcmp(buf, pkt, pkt_len) == 0) {
            start = pkt_len;  // skip the echo
        }
        if (got - start >= 6 && buf[start] == 0xFF && buf[start + 1] == 0xFF && buf[start + 2] == id) {
            int rlen = buf[start + 3];
            if (got - start >= rlen + 4) {
                uint8_t s = 0;
                for (int i = 2; i < rlen + 3; i++) s += buf[start + i];
                ok = static_cast<uint8_t>(~s) == buf[start + rlen + 3] && rlen - 2 == resp_len;
                if (ok && resp_len > 0) {
                    memcpy(resp, buf + start + 5, resp_len);
                }
                break;
            }
        }
    }
    xSemaphoreGive(bus_mutex);
    return ok;
}

bool readRegs(uint8_t id, uint8_t addr, uint8_t* out, int len) {
    const uint8_t p[2] = {addr, static_cast<uint8_t>(len)};
    return transact(id, kInstRead, p, 2, out, len);
}

bool writeRegs(uint8_t id, uint8_t addr, const uint8_t* data, int len) {
    uint8_t p[16];
    p[0] = addr;
    memcpy(p + 1, data, len);
    return transact(id, kInstWrite, p, len + 1, nullptr, 0);
}

bool writePosition(const Axis& a, float deg) {
    int raw = a.zero + static_cast<int>(lroundf(deg * kRawPerDeg));
    raw = raw < 0 ? 0 : (raw > 1000 ? 1000 : raw);
    const uint8_t d[6] = {static_cast<uint8_t>(raw >> 8), static_cast<uint8_t>(raw), 0, kStepMs, 0, 0};
    return writeRegs(a.id, kGoalPosition, d, 6);
}

bool setTorque(const Axis& a, bool on) {
    const uint8_t v = on ? 1 : 0;
    return writeRegs(a.id, kTorqueEnable, &v, 1);
}

struct Feedback {
    float angle;
    int load;
    int volt_mv;
    int temp_c;
};

bool readFeedback(const Axis& a, Feedback* f) {
    uint8_t d[8];
    if (!readRegs(a.id, kPresentPosition, d, sizeof(d))) {
        return false;
    }
    int pos = (d[0] << 8) | d[1];
    int load = (d[4] << 8) | d[5];
    if (load & (1 << 10)) load = -(load & ~(1 << 10));
    f->angle = (pos - a.zero) / kRawPerDeg;
    f->load = load;
    f->volt_mv = d[6] * 100;
    f->temp_c = d[7];
    return true;
}

void setVmEnable(bool on) {
    // PY32 pin 0: output, pull-up, level
    py32.update8(0x03, 0x01, 0x01);
    py32.update8(0x0B, 0x01, 0x00);
    py32.update8(0x09, 0x01, 0x01);
    py32.update8(0x05, 0x01, on ? 0x01 : 0x00);
}

bool vmEnabled() {
    uint8_t mode = 0, out = 0;
    return py32.read8(0x03, &mode) == ESP_OK && py32.read8(0x05, &out) == ESP_OK && (mode & 1) && (out & 1);
}

void powerOff() {
    xSemaphoreTake(mutex, portMAX_DELAY);
    for (Axis& a : axes) {
        setTorque(a, false);
        a.torque = false;
        a.vel = 0;
    }
    lifted = false;
    xSemaphoreGive(mutex);
    setVmEnable(false);
    vm_off_ms = esp_timer_get_time() / 1000;
    powered = false;
}

void fault(const Axis& a, const char* kind) {
    ESP_LOGW(TAG, "%s fault: %s, power cut", a.name, kind);
    wanted = false;  // stays off until servo.power on
    powerOff();
    cJSON* f = cJSON_CreateObject();
    cJSON_AddStringToObject(f, "servo", a.name);
    cJSON_AddStringToObject(f, "kind", kind);
    events::emit("servo.fault", f);
}

// ---- background: interpolation and protection ----

void step() {
    if (!powered) return;
    xSemaphoreTake(mutex, portMAX_DELAY);
    for (Axis& a : axes) {
        if (!a.torque || a.command == a.target) {
            a.vel = 0;
            continue;
        }
        // Starts at min_speed, accelerates to speed, decelerates back to min_speed at the target, stops.
        // A target change mid-move is followed from the current velocity (braking first if moving away).
        constexpr float dt = kStepMs / 1000.0f;
        float vmax = a.speed < kMaxSpeed ? a.speed : kMaxSpeed;
        float vmin = a.min_speed < vmax ? a.min_speed : vmax;
        float diff = a.target - a.command;
        float dir = diff > 0 ? 1.0f : -1.0f;
        float dist = fabsf(diff);
        float v = a.vel * dir;  // speed toward the target (negative = moving away)
        float max_dv = kAccel * dt;
        if (v < -vmin) {
            v += max_dv;  // moving away fast: brake first
        } else {
            if (v < vmin) v = vmin;
            float want = fminf(vmax, sqrtf(vmin * vmin + 2 * kAccel * dist));
            float dv = want - v;
            v += fabsf(dv) <= max_dv ? dv : (dv > 0 ? max_dv : -max_dv);
            if (v < vmin) v = vmin;
        }
        a.vel = v * dir;
        if (v > 0 && v * dt >= dist) {
            a.command = a.target;  // arrives this step
            a.vel = 0;
        } else {
            a.command += a.vel * dt;
        }
        if (!writePosition(a, a.command)) {
            ESP_LOGW(TAG, "%s write failed", a.name);
        }
    }
    xSemaphoreGive(mutex);
}

void monitor() {
    if (!powered) return;
    int64_t now = esp_timer_get_time() / 1000;
    for (Axis& a : axes) {
        Feedback f;
        if (!a.torque) continue;
        if (!readFeedback(a, &f)) {
            fault(a, "bus_error");
            return;
        }
        if (f.temp_c >= kOverheatC) {
            fault(a, "overheat");
            return;
        }
        a.overload_count = abs(f.load) >= kOverloadLoad ? a.overload_count + 1 : 0;
        if (a.overload_count >= kOverloadSamples) {
            fault(a, "overload");
            return;
        }
        bool settled = a.command == a.target;
        if (settled && fabsf(f.angle - a.target) > kStallErrorDeg) {
            if (a.stall_since == 0) a.stall_since = now;
            if (now - a.stall_since >= kStallMs) {
                fault(a, "stall");
                return;
            }
        } else {
            a.stall_since = 0;
        }
    }
}

// ---- methods ----

bool servoSource(cJSON* obj) {
    if (!ready) return false;
    for (Axis& a : axes) {
        cJSON* s = cJSON_AddObjectToObject(obj, a.name);
        Feedback f;
        bool ok = readFeedback(a, &f);
        xSemaphoreTake(mutex, portMAX_DELAY);
        float target = a.target;
        bool torque = a.torque;
        xSemaphoreGive(mutex);
        if (ok) {
            cJSON_AddNumberToObject(s, "angle", roundf(f.angle * 10) / 10.0);
            cJSON_AddNumberToObject(s, "temp_c", f.temp_c);
            cJSON_AddNumberToObject(s, "volt_mv", f.volt_mv);
            cJSON_AddNumberToObject(s, "load", f.load);
        } else {
            cJSON_AddNullToObject(s, "angle");
        }
        cJSON_AddNumberToObject(s, "target", roundf(target * 10) / 10.0);
        cJSON_AddBoolToObject(s, "torque", torque);
        cJSON_AddBoolToObject(s, "powered", powered.load());
    }
    return true;
}

cJSON* get(const cJSON*, rpc::Error* err) {
    cJSON* obj = cJSON_CreateObject();
    if (!servoSource(obj)) {
        cJSON_Delete(obj);
        return rpc::fail(err, rpc::kNotAvailable, "servo not available");
    }
    return obj;
}

// Powers on and holds the current position. Called with power_mutex held.
bool powerOnLocked(rpc::Error* err) {
    if (powered) {
        return true;  // already on and holding
    }
    // Turning VM_EN back on right after cutting it left the servos half-reset: they reported a
    // bogus position, and holding it slammed both axes to their mechanical ends. Wait for a full reset.
    if (vm_off_ms != 0) {
        int64_t off_for = esp_timer_get_time() / 1000 - vm_off_ms;
        if (off_for < kVmOffMinMs) vTaskDelay(pdMS_TO_TICKS(kVmOffMinMs - off_for));
    }
    setVmEnable(true);
    // The servos answer only after booting, 0.3-1 s after VM_EN (measured). Trust the position only
    // after several consecutive, consistent and plausible readings of both axes.
    Feedback fb[2], prev[2];
    int stable = 0;
    const int64_t deadline = esp_timer_get_time() + kBootTimeoutMs * 1000LL;
    while (stable < kStableReads && esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(50));
        bool ok = true;
        for (int i = 0; i < 2 && ok; i++) {
            ok = readFeedback(axes[i], &fb[i]) && fb[i].volt_mv >= 4000 && fb[i].temp_c > 0 && fb[i].temp_c < 100;
        }
        bool same = ok && stable > 0 && fabsf(fb[0].angle - prev[0].angle) <= kStableDeg &&
                    fabsf(fb[1].angle - prev[1].angle) <= kStableDeg;
        stable = !ok ? 0 : (same ? stable + 1 : 1);
        if (ok) {
            prev[0] = fb[0];
            prev[1] = fb[1];
        }
    }
    if (stable < kStableReads) {
        powerOff();
        rpc::fail(err, rpc::kHardwareError, "servo not responding");
        return false;
    }
    for (int i = 0; i < 2; i++) {
        if (fb[i].angle < axes[i].min_deg - kPowerOnMarginDeg || fb[i].angle > axes[i].max_deg + kPowerOnMarginDeg) {
            ESP_LOGW(TAG, "%s reads %.1f deg, outside the limits; not powering on", axes[i].name, fb[i].angle);
            powerOff();
            rpc::fail(err, rpc::kHardwareError, "servo position reading out of range");
            return false;
        }
    }
    // Hold the current position: goal = present position, then torque on
    for (int i = 0; i < 2; i++) {
        Axis& a = axes[i];
        const Feedback& f = fb[i];
        float hold = f.angle < a.min_deg ? a.min_deg : (f.angle > a.max_deg ? a.max_deg : f.angle);
        xSemaphoreTake(mutex, portMAX_DELAY);
        a.command = a.target = hold;
        a.speed = a.min_speed = kGentleSpeed;  // gentle when easing back inside the limits
        a.overload_count = 0;
        a.stall_since = 0;
        writePosition(a, f.angle);  // exact present position, no jump
        a.command = f.angle;        // then step toward the clamped hold angle
        a.vel = 0;
        a.torque = setTorque(a, true);
        xSemaphoreGive(mutex);
    }
    powered = true;
    return true;
}

cJSON* power(const cJSON* params, rpc::Error* err) {
    bool on = false;
    if (!ready) return rpc::fail(err, rpc::kNotAvailable, "servo not available");
    if (!rpc::getBool(params, "on", &on, err)) return nullptr;
    wanted = on;
    auto_failed = false;
    xSemaphoreTake(power_mutex, portMAX_DELAY);
    bool ok = true;
    if (on) {
        ok = powerOnLocked(err);
    } else {
        powerOff();
    }
    xSemaphoreGive(power_mutex);
    return ok ? cJSON_CreateObject() : nullptr;
}

void autoTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(kAutoBootDelayMs));
    while (true) {
        bool want = wanted && !idle_off;
        xSemaphoreTake(power_mutex, portMAX_DELAY);
        if (want && !powered && !auto_failed) {
            ESP_LOGI(TAG, "auto power on");
            rpc::Error err;
            if (!powerOnLocked(&err)) {
                auto_failed = true;
                ESP_LOGW(TAG, "auto power on failed: %s", err.message ? err.message : "?");
                events::error("servo_power", err.message ? err.message : "power on failed");
            }
        } else if (!want && powered) {
            ESP_LOGI(TAG, "auto power off (%s)", idle_off ? "idle" : "not wanted");
            powerOff();
        }
        xSemaphoreGive(power_mutex);
        vTaskDelay(pdMS_TO_TICKS(kAutoPeriodMs));
    }
}


bool moveTo(float yaw, float pitch, bool has_yaw, bool has_pitch, float min_speed, float speed, rpc::Error* err) {
    if (!ready) {
        rpc::fail(err, rpc::kNotAvailable, "servo not available");
        return false;
    }
    if (!powered || !axes[0].torque || !axes[1].torque) {
        rpc::fail(err, rpc::kServoUnavailable, "servo power off (servo.power on first)");
        return false;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (has_yaw) {
        axes[0].target = yaw;
        axes[0].speed = speed;
        axes[0].min_speed = min_speed;
    }
    if (has_pitch) {
        axes[1].target = pitch;
        axes[1].speed = speed;
        axes[1].min_speed = min_speed;
    }
    xSemaphoreGive(mutex);
    return true;
}

cJSON* move(const cJSON* params, rpc::Error* err) {
    double yaw = 0, pitch = 0, speed = kDefaultMaxSpeed, min_speed = kDefaultMinSpeed;
    bool has_yaw = params && cJSON_GetObjectItemCaseSensitive(params, "yaw");
    bool has_pitch = params && cJSON_GetObjectItemCaseSensitive(params, "pitch");
    if (!has_yaw && !has_pitch) return rpc::fail(err, rpc::kInvalidParams, "yaw and/or pitch required");
    if (!rpc::getOptNumber(params, "yaw", axes[0].min_deg, axes[0].max_deg, &yaw, err) ||
        !rpc::getOptNumber(params, "pitch", axes[1].min_deg, axes[1].max_deg, &pitch, err) ||
        !rpc::getOptNumber(params, "speed", kMinSpeed, kMaxSpeed, &speed, err) ||
        !rpc::getOptNumber(params, "min_speed", kMinSpeed, kMaxSpeed, &min_speed, err)) {
        return nullptr;
    }
    if (!moveTo(static_cast<float>(yaw), static_cast<float>(pitch), has_yaw, has_pitch,
                static_cast<float>(min_speed), static_cast<float>(speed), err)) {
        return nullptr;
    }
    return cJSON_CreateObject();
}

cJSON* home(const cJSON* params, rpc::Error* err) {
    double speed = kDefaultMaxSpeed, min_speed = kDefaultMinSpeed;
    if (!rpc::getOptNumber(params, "speed", kMinSpeed, kMaxSpeed, &speed, err) ||
        !rpc::getOptNumber(params, "min_speed", kMinSpeed, kMaxSpeed, &min_speed, err)) {
        return nullptr;
    }
    if (!moveTo(0, axes[1].min_deg, true, true, static_cast<float>(min_speed), static_cast<float>(speed), err)) {
        return nullptr;
    }
    return cJSON_CreateObject();
}

cJSON* rotate(const cJSON*, rpc::Error* err) {
    // Wheel mode rewrites the servo's EEPROM angle limits; not enabled in this firmware yet.
    return rpc::fail(err, rpc::kNotAvailable, "servo.rotate not implemented");
}

const rpc::Method kMethods[] = {
    {"servo.get", get, nullptr},     {"servo.move", move, nullptr},     {"servo.home", home, nullptr},
    {"servo.power", power, nullptr}, {"servo.rotate", rotate, nullptr},
};

void loadZeroPositions() {
    // Calibrated zero positions written by the official firmware (NVS namespace "servo"); never erased here
    if (nvs_flash_init() != ESP_OK) {
        ESP_LOGW(TAG, "NVS not usable, default zero positions");
        return;
    }
    nvs_handle_t h;
    if (nvs_open("servo", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    const char* keys[2] = {"zero_pos_1", "zero_pos_2"};
    for (int i = 0; i < 2; i++) {
        int32_t v = -1;
        if (nvs_get_i32(h, keys[i], &v) == ESP_OK && v >= 0 && v <= 1000) {
            axes[i].zero = v;
        }
    }
    nvs_close(h);
}

}  // namespace

void init() {
    bus_mutex = xSemaphoreCreateMutex();
    mutex = xSemaphoreCreateMutex();
    power_mutex = xSemaphoreCreateMutex();
    loadZeroPositions();
    ESP_LOGI(TAG, "zero positions: yaw %d, pitch %d", axes[0].zero, axes[1].zero);

    uart_config_t cfg = {};
    cfg.baud_rate = kBaud;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    if (uart_driver_install(kUart, 512, 0, 0, nullptr, 0) != ESP_OK || uart_param_config(kUart, &cfg) != ESP_OK ||
        uart_set_pin(kUart, kTxPin, kRxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        events::error("sensor_init", "servo uart");
        return;
    }
    if (!py32.open(0x6F, 100000)) {
        events::error("sensor_init", "servo power (py32)");
        return;
    }
    ready = true;
    // Boot leaves the servos as they are; torque state is unknown until servo.power
    powered = false;
    ESP_LOGI(TAG, "VM_EN currently %s", vmEnabled() ? "on" : "off");
}

void registerMethods() {
    rpc::registerMethods(kMethods, sizeof(kMethods) / sizeof(kMethods[0]));
}

void start() {
    if (!ready) return;
    poller::add("servo.step", step, kStepMs);
    poller::add("servo.monitor", monitor, 100);
    streams::add("servo", servoSource);
    xTaskCreate(autoTask, "servo.auto", 4096, nullptr, 4, nullptr);
}

void setIdleOff(bool off) {
    idle_off = off;
}

void reactLift(bool on) {
    if (!ready) return;
    xSemaphoreTake(mutex, portMAX_DELAY);
    Axis& p = axes[1];
    if (on && !lifted && powered && p.torque) {
        lift_base = p.target;
        lift_to = p.target + kLiftDeg > p.max_deg ? p.max_deg : p.target + kLiftDeg;
        p.target = lift_to;
        p.speed = p.min_speed = kGentleSpeed;
        lifted = true;
    } else if (!on && lifted) {
        if (powered && p.torque && p.target == lift_to) {
            p.target = lift_base;
            p.speed = p.min_speed = kGentleSpeed;
        }
        lifted = false;
    }
    xSemaphoreGive(mutex);
}

bool moving() {
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool m = axes[0].command != axes[0].target || axes[1].command != axes[1].target;
    xSemaphoreGive(mutex);
    return m;
}

}  // namespace servo
