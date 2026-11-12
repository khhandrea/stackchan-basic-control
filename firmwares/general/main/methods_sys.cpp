#include "methods_sys.h"

#include <cstdio>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "link.h"
#include "notify.h"
#include "rpc.h"

namespace sys {
namespace {

const char* resetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "power_on";
        case ESP_RST_EXT: return "external";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "int_wdt";
        case ESP_RST_TASK_WDT: return "task_wdt";
        case ESP_RST_WDT: return "wdt";
        case ESP_RST_DEEPSLEEP: return "deep_sleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "sdio";
        case ESP_RST_USB: return "usb";
        case ESP_RST_JTAG: return "jtag";
        case ESP_RST_EFUSE: return "efuse";
        case ESP_RST_PWR_GLITCH: return "power_glitch";
        case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
        default: return "unknown";
    }
}

void macString(char* out, size_t cap) {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, cap, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// esp_app_desc date "Nov 13 2026" and time "12:34:56" → "2026-11-13T12:34:56".
void buildDate(char* out, size_t cap) {
    const esp_app_desc_t* app = esp_app_get_description();
    static const char kMonths[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {};
    int day = 0;
    int year = 0;
    if (sscanf(app->date, "%3s %d %d", mon, &day, &year) != 3) {
        snprintf(out, cap, "%s %s", app->date, app->time);
        return;
    }
    const char* found = strstr(kMonths, mon);
    int month = found != nullptr ? static_cast<int>(found - kMonths) / 3 + 1 : 0;
    snprintf(out, cap, "%04d-%02d-%02dT%s", year, month, day, app->time);
}

cJSON* ping(const cJSON*, rpc::Error*) {
    cJSON* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "pong", true);
    return result;
}

cJSON* info(const cJSON*, rpc::Error*) {
    char mac[18];
    char date[32];
    macString(mac, sizeof(mac));
    buildDate(date, sizeof(date));

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "fw_version", esp_app_get_description()->version);
    cJSON_AddStringToObject(result, "build_date", date);
    cJSON_AddStringToObject(result, "mac", mac);
    return result;
}

cJSON* emptyResult(const cJSON*, rpc::Error*) {
    return cJSON_CreateObject();
}

void rebootNow() {
    hostlink::flush();
    esp_restart();
}

cJSON* listMethods(const cJSON*, rpc::Error*) {
    cJSON* result = cJSON_CreateArray();
    for (const rpc::Method& m : rpc::methods()) {
        cJSON_AddItemToArray(result, cJSON_CreateString(m.name));
    }
    return result;
}

const rpc::Method kMethods[] = {
    {"sys.ping", ping, nullptr},
    {"sys.info", info, nullptr},
    {"sys.reboot", emptyResult, rebootNow},
    {"rpc.list", listMethods, nullptr},
};

}  // namespace

void registerMethods() {
    rpc::registerMethods(kMethods, sizeof(kMethods) / sizeof(kMethods[0]));
}

void sendBootEvent() {
    char mac[18];
    macString(mac, sizeof(mac));

    cJSON* fields = cJSON_CreateObject();
    cJSON_AddStringToObject(fields, "fw_version", esp_app_get_description()->version);
    cJSON_AddStringToObject(fields, "reset_reason", resetReasonName(esp_reset_reason()));
    cJSON_AddStringToObject(fields, "mac", mac);
    cJSON_AddNumberToObject(fields, "free_heap", esp_get_free_heap_size());
    notify::event("boot", fields);
}

}  // namespace sys
