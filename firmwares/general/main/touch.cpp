#include "touch.h"

#include <atomic>

#include "board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "i2c_bus.h"
#include "idle.h"
#include "modal.h"
#include "poller.h"
#include "rpc.h"
#include "streams.h"

namespace touch {
namespace {

const char* TAG = "touch";

constexpr int kScreenLongPressMs = 600;
constexpr int kHeadTapMaxMs = 500;
constexpr int kHeadLongPressMs = 1000;

// Zones front to back; the Si12T channels run the other way (channel 0 = back), checked on the device
constexpr const char* kZones[3] = {"front", "middle", "back"};
constexpr int kZoneChannel[3] = {2, 1, 0};

i2c::Device si12t;
SemaphoreHandle_t mutex = xSemaphoreCreateMutex();

struct Screen {
    bool pressed = false;
    int x = -1, y = -1;
    int start_x = 0, start_y = 0;
    int64_t start_ms = 0;
    bool long_sent = false;
} screen;

struct Zone {
    bool touched = false;
    uint8_t level = 0;
    int64_t start_ms = 0;
    bool long_sent = false;
} zones[3];

std::atomic<uint32_t> count{0};
std::atomic<int64_t> last_ms{0};

int64_t nowMs() {
    return esp_timer_get_time() / 1000;
}

void countEvent(int64_t now) {
    count++;
    last_ms = now;
}

void emitScreen(const char* kind, int x, int y, int64_t now) {
    countEvent(now);
    cJSON* f = cJSON_CreateObject();
    cJSON_AddStringToObject(f, "kind", kind);
    cJSON_AddNumberToObject(f, "x", x);
    cJSON_AddNumberToObject(f, "y", y);
    events::emit("touch.screen", f);
}

void emitHead(int zone, const char* kind, int64_t now) {
    countEvent(now);
    cJSON* f = cJSON_CreateObject();
    cJSON_AddStringToObject(f, "zone", kZones[zone]);
    cJSON_AddStringToObject(f, "kind", kind);
    events::emit("touch.head", f);
}

void pollScreen() {
    uint8_t d[5];
    if (board::ft6336().read(0x02, d, sizeof(d)) != ESP_OK) {
        return;
    }
    int points = d[0] & 0x0F;
    bool pressed = points >= 1 && points <= 2;
    int x = ((d[1] & 0x0F) << 8) | d[2];
    int y = ((d[3] & 0x0F) << 8) | d[4];
    int64_t now = nowMs();

    xSemaphoreTake(mutex, portMAX_DELAY);
    Screen s = screen;
    if (pressed) {
        s.x = x;
        s.y = y;
        if (!s.pressed) {
            idle::activity();
            s.start_x = x;
            s.start_y = y;
            s.start_ms = now;
            s.long_sent = false;
        }
    }
    bool was_pressed = s.pressed;
    s.pressed = pressed;
    bool send_long = pressed && !s.long_sent && now - s.start_ms >= kScreenLongPressMs;
    if (send_long) {
        s.long_sent = true;
    }
    bool send_tap = !pressed && was_pressed && !s.long_sent;
    screen = s;
    xSemaphoreGive(mutex);

    if (send_long) {
        emitScreen("long_press", s.start_x, s.start_y, now);
    }
    if (send_tap && !modal::handleTap(s.start_x, s.start_y)) {
        emitScreen("tap", s.start_x, s.start_y, now);
    }
}

void pollHead() {
    uint8_t out;
    if (si12t.read8(0x10, &out) != ESP_OK) {
        return;
    }
    int64_t now = nowMs();
    for (int i = 0; i < 3; i++) {
        uint8_t level = (out >> (2 * kZoneChannel[i])) & 0x03;
        bool touched = level >= 1;
        xSemaphoreTake(mutex, portMAX_DELAY);
        Zone z = zones[i];
        z.level = level;
        bool started = touched && !z.touched;
        bool ended = !touched && z.touched;
        if (started) {
            z.start_ms = now;
            z.long_sent = false;
        }
        bool send_long = touched && !z.long_sent && now - z.start_ms >= kHeadLongPressMs;
        if (send_long) {
            z.long_sent = true;
        }
        bool send_tap = ended && !z.long_sent && now - z.start_ms < kHeadTapMaxMs;
        z.touched = touched;
        zones[i] = z;
        xSemaphoreGive(mutex);

        if (started) {
            idle::activity();
            emitHead(i, "start", now);
        }
        if (send_long) emitHead(i, "long_press", now);
        if (ended) emitHead(i, "end", now);
        if (send_tap) emitHead(i, "tap", now);
    }
}

bool touchSource(cJSON* obj) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    Screen s = screen;
    Zone z[3] = {zones[0], zones[1], zones[2]};
    xSemaphoreGive(mutex);
    cJSON* head = cJSON_AddObjectToObject(obj, "head");
    cJSON* level = cJSON_CreateArray();
    for (int i = 0; i < 3; i++) {
        cJSON_AddBoolToObject(head, kZones[i], z[i].touched);
        cJSON_AddItemToArray(level, cJSON_CreateNumber(z[i].level));
    }
    cJSON_AddItemToObject(head, "level", level);
    cJSON* scr = cJSON_AddObjectToObject(obj, "screen");
    cJSON_AddBoolToObject(scr, "pressed", s.pressed);
    cJSON_AddNumberToObject(scr, "x", s.pressed ? s.x : -1);
    cJSON_AddNumberToObject(scr, "y", s.pressed ? s.y : -1);
    cJSON_AddNumberToObject(obj, "count", count.load());
    cJSON_AddNumberToObject(obj, "last_ms", static_cast<double>(last_ms.load()));
    return true;
}

cJSON* get(const cJSON*, rpc::Error*) {
    cJSON* obj = cJSON_CreateObject();
    touchSource(obj);
    return obj;
}

const rpc::Method kMethods[] = {
    {"touch.get", get, nullptr},
};

void initSi12t() {
    // Same setup as the official firmware: all channels on, CTRL2 0x0F -> 0x07, CTRL1 0x22, sensitivity low 3
    for (uint8_t reg = 0x0A; reg <= 0x0F; reg++) {
        si12t.write8(reg, 0x00);
    }
    si12t.write8(0x09, 0x0F);
    si12t.write8(0x09, 0x07);
    si12t.write8(0x08, 0x22);
    for (uint8_t reg = 0x02; reg <= 0x06; reg++) {
        si12t.write8(reg, 0x33);
    }
}

}  // namespace

void init() {
    if (si12t.open(0x68, 100000)) {
        initSi12t();
    } else {
        ESP_LOGE(TAG, "Si12T not found");
        events::error("sensor_init", "si12t");
    }
}

void registerMethods() {
    rpc::registerMethods(kMethods, sizeof(kMethods) / sizeof(kMethods[0]));
}

void start() {
    if (board::ft6336().isOpen()) {
        poller::add("touch.screen", pollScreen, 20);
    }
    if (si12t.isOpen()) {
        poller::add("touch.head", pollHead, 20);
    }
    streams::add("touch", touchSource);
}

bool headTouched() {
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool any = zones[0].touched || zones[1].touched || zones[2].touched;
    xSemaphoreGive(mutex);
    return any;
}

}  // namespace touch
