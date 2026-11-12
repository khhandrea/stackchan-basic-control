#include "rtc.h"

#include <cstdio>
#include <ctime>
#include <sys/time.h>

#include "esp_log.h"
#include "events.h"
#include "i2c_bus.h"
#include "rpc.h"

namespace rtc {
namespace {

const char* TAG = "rtc";

constexpr uint8_t kSeconds = 0x02;  // seconds, minutes, hours, day, weekday, month, year (BCD)

i2c::Device bm8563;
bool ready = false;

uint8_t fromBcd(uint8_t v) {
    return static_cast<uint8_t>((v >> 4) * 10 + (v & 0x0F));
}

uint8_t toBcd(int v) {
    return static_cast<uint8_t>(((v / 10) << 4) | (v % 10));
}

// Returns false if the read failed. *valid = false when the RTC lost power (VL bit).
bool readTime(struct tm* t, bool* valid) {
    uint8_t r[7];
    if (bm8563.read(kSeconds, r, sizeof(r)) != ESP_OK) {
        return false;
    }
    *valid = !(r[0] & 0x80);
    *t = {};
    t->tm_sec = fromBcd(r[0] & 0x7F);
    t->tm_min = fromBcd(r[1] & 0x7F);
    t->tm_hour = fromBcd(r[2] & 0x3F);
    t->tm_mday = fromBcd(r[3] & 0x3F);
    t->tm_wday = r[4] & 0x07;
    t->tm_mon = fromBcd(r[5] & 0x1F) - 1;
    t->tm_year = fromBcd(r[6]) + ((r[5] & 0x80) ? 0 : 100);  // century bit 0 = 20xx
    return true;
}

void setSystemClock(const struct tm& t) {
    struct tm copy = t;
    setenv("TZ", "UTC0", 1);
    tzset();
    struct timeval tv = {mktime(&copy), 0};
    settimeofday(&tv, nullptr);
}

cJSON* get(const cJSON*, rpc::Error* err) {
    struct tm t;
    bool valid;
    if (!ready || !readTime(&t, &valid)) {
        return rpc::fail(err, rpc::kNotAvailable, "rtc not available");
    }
    char iso[64];
    snprintf(iso, sizeof(iso), "%04d-%02d-%02dT%02d:%02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "iso", iso);
    cJSON_AddBoolToObject(obj, "valid", valid);
    return obj;
}

cJSON* set(const cJSON* params, rpc::Error* err) {
    const char* iso = rpc::getString(params, "iso", err);
    if (iso == nullptr) {
        return nullptr;
    }
    struct tm t = {};
    if (sscanf(iso, "%4d-%2d-%2dT%2d:%2d:%2d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min,
               &t.tm_sec) != 6 ||
        t.tm_year < 2000 || t.tm_year > 2099 || t.tm_mon < 1 || t.tm_mon > 12 || t.tm_mday < 1 || t.tm_mday > 31 ||
        t.tm_hour > 23 || t.tm_min > 59 || t.tm_sec > 59) {
        return rpc::fail(err, rpc::kInvalidParams, "iso: YYYY-MM-DDTHH:MM:SS (2000-2099)");
    }
    if (!ready) {
        return rpc::fail(err, rpc::kNotAvailable, "rtc not available");
    }
    t.tm_year -= 1900;
    t.tm_mon -= 1;
    struct tm normalized = t;
    setenv("TZ", "UTC0", 1);
    tzset();
    mktime(&normalized);  // fills tm_wday
    const uint8_t r[7] = {toBcd(t.tm_sec),  toBcd(t.tm_min),        toBcd(t.tm_hour),
                          toBcd(t.tm_mday), static_cast<uint8_t>(normalized.tm_wday), toBcd(t.tm_mon + 1),
                          toBcd(t.tm_year - 100)};
    if (bm8563.write(kSeconds, r, sizeof(r)) != ESP_OK) {
        return rpc::fail(err, rpc::kHardwareError, "rtc write failed");
    }
    setSystemClock(t);
    return cJSON_CreateObject();
}

const rpc::Method kMethods[] = {
    {"time.get", get, nullptr},
    {"time.set", set, nullptr},
};

}  // namespace

void init() {
    if (!bm8563.open(0x51)) {
        ESP_LOGE(TAG, "BM8563 not found");
        events::error("sensor_init", "bm8563");
        return;
    }
    bm8563.write8(0x00, 0x00);  // control 1: normal, clock running
    ready = true;
    struct tm t;
    bool valid;
    if (readTime(&t, &valid) && valid) {
        setSystemClock(t);
    }
}

void registerMethods() {
    rpc::registerMethods(kMethods, sizeof(kMethods) / sizeof(kMethods[0]));
}

}  // namespace rtc
