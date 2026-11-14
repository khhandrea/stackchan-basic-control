#include "power.h"

#include <atomic>
#include <cstring>

#include "driver/temperature_sensor.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "events.h"
#include "i2c_bus.h"
#include "idle.h"
#include "link.h"
#include "poller.h"
#include "rpc.h"
#include "streams.h"

namespace power {
namespace {

const char* TAG = "power";

// ---- AXP2101 registers ----
constexpr uint8_t kStatus1 = 0x00;       // bit5 VBUS good, bit3 battery present
constexpr uint8_t kStatus2 = 0x01;       // bit6:5 current direction, bit2:0 charge state
constexpr uint8_t kPowerOff = 0x10;      // bit0 = power off
constexpr uint8_t kIrqEnable2 = 0x41;
constexpr uint8_t kIrqStatus2 = 0x49;    // bit3 PKEY short press, bit1 negative edge, bit0 positive edge
constexpr uint8_t kVbat = 0x34;          // 14-bit mV, then TS, VBUS, VSYS, die temp
constexpr uint8_t kLdoEnable = 0x90;     // bit7 DLDO1 (LCD backlight)
constexpr uint8_t kDldo1Voltage = 0x99;
constexpr uint8_t kChargeCurrent = 0x62;
constexpr uint8_t kChargeCtrl = 0x18;       // bit1 cell battery charge enable
constexpr uint8_t kBatteryPercent = 0xA4;

constexpr uint8_t kPkeyShortPress = 1 << 3;
constexpr uint8_t kPkeyNegativeEdge = 1 << 1;
constexpr uint8_t kPkeyPositiveEdge = 1 << 0;

// ---- INA226 (battery, 10 mOhm shunt) ----
constexpr uint8_t kInaConfig = 0x00;
constexpr uint8_t kInaShunt = 0x01;  // signed, 2.5 uV/LSB → 0.25 mA/LSB with 10 mOhm

// Storage charge mode (default): on USB, charge only below 40 % and stop at 60 %, so an always-plugged
// battery does not sit full and warm. Full mode: the charger runs normally (4.2 V).
constexpr int kStorageLowPct = 40;
constexpr int kStorageHighPct = 60;

constexpr int kLowBatteryPct = 10;
constexpr int kLowBatteryClearPct = 15;

i2c::Device axp;
i2c::Device ina;
temperature_sensor_handle_t tsens = nullptr;

enum class ChargeDir { Standby = 0, Charging = 1, Discharging = 2 };

struct State {
    bool vbus = false;
    bool charging = false;
    bool low_battery_sent = false;
} state;

std::atomic<bool> button_pressed{false};
std::atomic<bool> button_poked{false};  // pressed since the last takeButtonPress()
std::atomic<int64_t> button_last_ms{0};
std::atomic<uint32_t> power_log_interval_s{0};
std::atomic<bool> full_charge{false};
bool charger_on = true;  // last value written to the charger enable bit
int64_t power_log_next_ms = 0;

int64_t nowMs() {
    return esp_timer_get_time() / 1000;
}

struct Reading {
    int batt_pct = -1;
    int batt_mv = 0;
    int batt_ma = 0;
    bool has_batt_ma = false;
    bool charging = false;
    bool vbus = false;
    int vbus_mv = 0;
    const char* charge_state = "unknown";
    float chip_temp_c = 0;
    float pmic_temp_c = 0;
};

const char* chargeStateName(uint8_t cs) {
    switch (cs & 7) {
        case 0: return "trickle";
        case 1: return "precharge";
        case 2: return "constant_current";
        case 3: return "constant_voltage";
        case 4: return "done";
        default: return "not_charging";
    }
}

bool read(Reading* r) {
    uint8_t st[2];
    uint8_t adc[10];
    uint8_t pct;
    if (axp.read(kStatus1, st, 2) != ESP_OK || axp.read(kVbat, adc, 10) != ESP_OK ||
        axp.read8(kBatteryPercent, &pct) != ESP_OK) {
        return false;
    }
    r->vbus = st[0] & (1 << 5);
    r->charging = ((st[1] >> 5) & 3) == static_cast<int>(ChargeDir::Charging);
    r->charge_state = chargeStateName(st[1]);
    r->batt_pct = pct <= 100 ? pct : -1;
    r->batt_mv = ((adc[0] & 0x3F) << 8) | adc[1];
    r->vbus_mv = ((adc[4] & 0x3F) << 8) | adc[5];
    int die_raw = ((adc[8] & 0x3F) << 8) | adc[9];
    r->pmic_temp_c = 22.0f + (7274 - die_raw) / 20.0f;

    uint8_t shunt[2];
    if (ina.isOpen() && ina.read(kInaShunt, shunt, 2) == ESP_OK) {
        int16_t raw = static_cast<int16_t>((shunt[0] << 8) | shunt[1]);
        r->batt_ma = raw / 4;
        r->has_batt_ma = true;
    }
    if (tsens != nullptr) {
        temperature_sensor_get_celsius(tsens, &r->chip_temp_c);
    }
    return true;
}

void addStatusFields(cJSON* obj, const Reading& r) {
    cJSON_AddNumberToObject(obj, "uptime_ms", static_cast<double>(nowMs()));
    cJSON_AddNumberToObject(obj, "batt_pct", r.batt_pct);
    cJSON_AddNumberToObject(obj, "batt_mv", r.batt_mv);
    if (r.has_batt_ma) {
        cJSON_AddNumberToObject(obj, "batt_ma", r.batt_ma);
    } else {
        cJSON_AddNullToObject(obj, "batt_ma");
    }
    cJSON_AddBoolToObject(obj, "charging", r.charging);
    cJSON_AddStringToObject(obj, "charge_state", r.charge_state);
    cJSON_AddBoolToObject(obj, "vbus", r.vbus);
    cJSON_AddNumberToObject(obj, "vbus_mv", r.vbus_mv);
    cJSON_AddNumberToObject(obj, "chip_temp_c", static_cast<int>(r.chip_temp_c * 10) / 10.0);
    cJSON_AddNumberToObject(obj, "pmic_temp_c", static_cast<int>(r.pmic_temp_c * 10) / 10.0);
    cJSON_AddNumberToObject(obj, "free_heap", esp_get_free_heap_size());
    cJSON_AddStringToObject(obj, "charge_mode", full_charge ? "full" : "storage");
    cJSON_AddBoolToObject(obj, "charger_on", charger_on);
    idle::addStatus(obj);
}

bool statusSource(cJSON* obj) {
    Reading r;
    if (!axp.isOpen() || !read(&r)) {
        return false;
    }
    addStatusFields(obj, r);
    return true;
}

// ---- polling: power state changes, button ----

void emitCharge(const char* s) {
    cJSON* f = cJSON_CreateObject();
    cJSON_AddStringToObject(f, "state", s);
    events::emit("power.charge", f);
}

void pollButton() {
    uint8_t irq;
    if (axp.read8(kIrqStatus2, &irq) != ESP_OK) {
        return;
    }
    uint8_t pkey = irq & (kPkeyShortPress | kPkeyNegativeEdge | kPkeyPositiveEdge);
    if (pkey == 0) {
        return;
    }
    axp.write8(kIrqStatus2, pkey);  // write 1 to clear
    if (pkey & kPkeyNegativeEdge) {
        button_pressed = true;
        button_poked = true;
        idle::activity();
    }
    if (pkey & kPkeyPositiveEdge) {
        button_pressed = false;
    }
    if (pkey & kPkeyShortPress) {
        button_last_ms = nowMs();
        cJSON* f = cJSON_CreateObject();
        cJSON_AddStringToObject(f, "kind", "press");
        events::emit("button", f);
    }
}

void setCharger(bool on) {
    if (axp.update8(kChargeCtrl, 0x02, on ? 0x02 : 0x00) == ESP_OK) {
        charger_on = on;
        ESP_LOGI(TAG, "charger %s", on ? "on" : "off");
    }
}

void applyChargePolicy(const Reading& r) {
    bool want = charger_on;
    if (full_charge || !r.vbus || r.batt_pct < 0) {
        want = true;  // full mode, or nothing to manage without USB / gauge
    } else if (r.batt_pct < kStorageLowPct) {
        want = true;
    } else if (r.batt_pct >= kStorageHighPct) {
        want = false;
    }
    if (want != charger_on) setCharger(want);
}

void pollPower() {
    Reading r;
    if (!read(&r)) {
        return;
    }
    applyChargePolicy(r);
    if (r.vbus != state.vbus) {
        state.vbus = r.vbus;
        cJSON* f = cJSON_CreateObject();
        cJSON_AddBoolToObject(f, "attached", r.vbus);
        events::emit("power.usb", f);
    }
    if (r.charging != state.charging) {
        state.charging = r.charging;
        if (r.charging) {
            emitCharge("start");
        } else {
            emitCharge(strcmp(r.charge_state, "done") == 0 ? "done" : "stop");
        }
    }
    bool discharging = !r.vbus && r.batt_pct >= 0;
    if (discharging && r.batt_pct <= kLowBatteryPct && !state.low_battery_sent) {
        state.low_battery_sent = true;
        cJSON* f = cJSON_CreateObject();
        cJSON_AddNumberToObject(f, "batt_pct", r.batt_pct);
        cJSON_AddNumberToObject(f, "batt_mv", r.batt_mv);
        events::emit("power.low_battery", f);
    } else if (r.vbus || r.batt_pct >= kLowBatteryClearPct) {
        state.low_battery_sent = false;
    }

    uint32_t interval = power_log_interval_s;
    int64_t now = nowMs();
    if (interval != 0 && now >= power_log_next_ms) {
        power_log_next_ms = now + interval * 1000;
        ESP_LOGI(TAG, "batt %d%% %d mV %d mA, vbus %d (%d mV), %s, chip %.1f C, pmic %.1f C", r.batt_pct, r.batt_mv,
                 r.batt_ma, r.vbus, r.vbus_mv, r.charge_state, r.chip_temp_c, r.pmic_temp_c);
    }
}

// ---- methods ----

cJSON* status(const cJSON*, rpc::Error* err) {
    cJSON* obj = cJSON_CreateObject();
    if (!statusSource(obj)) {
        cJSON_Delete(obj);
        return rpc::fail(err, rpc::kHardwareError, "pmic read failed");
    }
    return obj;
}

void powerOffNow() {
    hostlink::flush();
    axp.update8(kPowerOff, 0x01, 0x01);
}

cJSON* powerOff(const cJSON*, rpc::Error* err) {
    if (!axp.isOpen()) {
        return rpc::fail(err, rpc::kNotAvailable, "pmic not available");
    }
    return cJSON_CreateObject();
}

bool buttonSource(cJSON* obj) {
    cJSON_AddBoolToObject(obj, "pressed", button_pressed.load());
    cJSON_AddNumberToObject(obj, "last_ms", static_cast<double>(button_last_ms.load()));
    return true;
}

cJSON* setChargeMode(const cJSON* params, rpc::Error* err) {
    const char* mode = rpc::getString(params, "mode", err);
    if (mode == nullptr) return nullptr;
    if (strcmp(mode, "full") == 0) {
        full_charge = true;
    } else if (strcmp(mode, "storage") == 0) {
        full_charge = false;
    } else {
        return rpc::fail(err, rpc::kInvalidParams, "mode: storage / full");
    }
    return cJSON_CreateObject();
}

cJSON* buttonGet(const cJSON*, rpc::Error*) {
    cJSON* obj = cJSON_CreateObject();
    buttonSource(obj);
    return obj;
}

cJSON* setLogLevel(const cJSON* params, rpc::Error* err) {
    const char* level = rpc::getString(params, "level", err);
    if (level == nullptr) {
        return nullptr;
    }
    static const struct {
        const char* name;
        esp_log_level_t level;
    } kLevels[] = {{"E", ESP_LOG_ERROR}, {"W", ESP_LOG_WARN},    {"I", ESP_LOG_INFO},
                   {"D", ESP_LOG_DEBUG}, {"V", ESP_LOG_VERBOSE}, {"none", ESP_LOG_NONE}};
    for (const auto& l : kLevels) {
        if (strcmp(l.name, level) == 0) {
            esp_log_level_set("*", l.level);
            return cJSON_CreateObject();
        }
    }
    return rpc::fail(err, rpc::kInvalidParams, "level: E / W / I / D / V / none");
}

cJSON* setPowerInterval(const cJSON* params, rpc::Error* err) {
    int interval = 0;
    if (!rpc::getInt(params, "interval_s", 0, 86400, &interval, err)) {
        return nullptr;
    }
    power_log_interval_s = static_cast<uint32_t>(interval);
    power_log_next_ms = 0;
    return cJSON_CreateObject();
}

const rpc::Method kMethods[] = {
    {"sys.status", status, nullptr},
    {"sys.power_off", powerOff, powerOffNow},
    {"power.set_charge_mode", setChargeMode, nullptr},
    {"button.get", buttonGet, nullptr},
    {"log.set_level", setLogLevel, nullptr},
    {"log.set_power_interval", setPowerInterval, nullptr},
};

void initAxp2101() {
    // Same setup as the official firmware (m5stack/StackChan v1.5.1, M5StackCoreS3Board::Pmic)
    uint8_t v = 0;
    axp.read8(kLdoEnable, &v);
    axp.write8(kLdoEnable, v | 0b10110100);
    axp.write8(0x97, 0b11110 - 2);
    axp.write8(0x69, 0b00110101);  // CHGLED
    axp.write8(0x30, 0b111111);    // ADC enable: VBAT, TS, VBUS, VSYS, die temp
    axp.write8(kLdoEnable, 0xBF);
    axp.write8(0x94, 33 - 5);      // ALDO3 3.3 V
    axp.write8(0x95, 33 - 5);      // ALDO4 3.3 V
    axp.write8(0x27, 0x00);        // power key timing
    axp.update8(kChargeCurrent, 0x1F, 13);  // 700 mA (official value)
    axp.update8(kIrqEnable2, kPkeyShortPress | kPkeyNegativeEdge | kPkeyPositiveEdge, 0xFF);
    axp.write8(kIrqStatus2, 0xFF);  // clear stale flags
    setBacklight(0);
}

}  // namespace

bool takeButtonPress() {
    return button_poked.exchange(false);
}

void setBacklight(int percent) {
    if (!axp.isOpen()) {
        return;
    }
    if (percent <= 0) {
        axp.update8(kLdoEnable, 0x80, 0x00);
        return;
    }
    if (percent > 100) {
        percent = 100;
    }
    // Official mapping: 1-100 → DLDO1 register 20-28
    axp.write8(kDldo1Voltage, static_cast<uint8_t>(20 + percent * 8 / 100));
    axp.update8(kLdoEnable, 0x80, 0x80);
}

void init() {
    if (axp.open(0x34)) {
        initAxp2101();
        Reading r;
        if (read(&r)) {
            state.vbus = r.vbus;
            state.charging = r.charging;
        }
        uint8_t ctrl = 0;
        if (axp.read8(kChargeCtrl, &ctrl) == ESP_OK) {
            charger_on = ctrl & 0x02;
        }
    } else {
        ESP_LOGE(TAG, "AXP2101 not found");
        events::error("sensor_init", "axp2101");
    }

    if (ina.open(0x41)) {
        const uint8_t config[2] = {0x45, 0x27};  // 16 averages, 1.1 ms conversions, continuous
        ina.write(kInaConfig, config, 2);
    } else {
        ESP_LOGW(TAG, "INA226 not found");
        events::error("sensor_init", "ina226");
    }

    temperature_sensor_config_t ts_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&ts_config, &tsens) == ESP_OK) {
        temperature_sensor_enable(tsens);
    } else {
        tsens = nullptr;
    }
}

void registerMethods() {
    rpc::registerMethods(kMethods, sizeof(kMethods) / sizeof(kMethods[0]));
}

void start() {
    if (!axp.isOpen()) {
        return;
    }
    poller::add("button", pollButton, 20);
    poller::add("power", pollPower, 200);
    streams::add("status", statusSource);
    streams::add("button", buttonSource);
}

}  // namespace power
