#include "modal.h"

#include <cstring>
#include <string>
#include <vector>

#include "esp_timer.h"
#include "events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lcd.h"
#include "rpc.h"

namespace modal {
namespace {

enum class Buttons { None, Ok, YesNo };

struct Line {
    size_t start;  // byte offset into body
    int count;     // code points
};

struct State {
    bool visible = false;
    std::string title;
    std::string body;
    Buttons buttons = Buttons::None;
    uint32_t timeout_ms = 0;
    int64_t shown_ms = 0;
};

SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
State state;          // guarded by mutex
State frame;          // copy for drawing
std::vector<Line> lines;  // wrapped body of frame
std::string wrapped_body;

// Box geometry
constexpr int kX = 16, kY = 24, kW = lcd::kWidth - 32, kH = lcd::kHeight - 48;
constexpr int kPad = 12;
constexpr int kButtonH = 34, kButtonW = 100;
constexpr int kTitleY = kY + kPad;
constexpr int kBodyY = kTitleY + gfx::kFontHeight + 10;
constexpr int kLineH = gfx::kFontHeight + 4;

constexpr uint16_t kBoxBg = gfx::rgb(24, 28, 40);
constexpr uint16_t kBorder = gfx::rgb(200, 200, 210);
constexpr uint16_t kText = gfx::rgb(255, 255, 255);
constexpr uint16_t kButtonBg = gfx::rgb(60, 110, 200);
constexpr uint16_t kButtonNoBg = gfx::rgb(90, 90, 100);

const char* buttonsName(Buttons b) {
    switch (b) {
        case Buttons::Ok: return "ok";
        case Buttons::YesNo: return "yes_no";
        default: return "none";
    }
}

struct ButtonRect {
    int x, y, w, h;
    const char* label;
    const char* result;
    uint16_t bg;
};

int buttonRects(Buttons b, ButtonRect* out) {
    int y = kY + kH - kPad - kButtonH;
    if (b == Buttons::Ok) {
        out[0] = {kX + (kW - kButtonW) / 2, y, kButtonW, kButtonH, "OK", "ok", kButtonBg};
        return 1;
    }
    if (b == Buttons::YesNo) {
        int gap = 16;
        int x0 = kX + (kW - 2 * kButtonW - gap) / 2;
        out[0] = {x0, y, kButtonW, kButtonH, "YES", "yes", kButtonBg};
        out[1] = {x0 + kButtonW + gap, y, kButtonW, kButtonH, "NO", "no", kButtonNoBg};
        return 2;
    }
    return 0;
}

// Wraps body into lines that fit the box width; prefers breaking at spaces, honors '\n'.
void wrap(const std::string& body) {
    lines.clear();
    const int max_w = kW - 2 * kPad;
    const char* base = body.c_str();
    const char* s = base;
    while (*s) {
        const char* line_start = s;
        const char* p = s;
        const char* last_space = nullptr;
        int count = 0, count_at_space = 0, width = 0;
        while (*p && *p != '\n') {
            const char* before = p;
            uint32_t cp = gfx::nextCodepoint(&p);
            int w = gfx::glyphWidth(cp);
            if (width + w > max_w && count > 0) {
                p = before;
                break;
            }
            width += w;
            count++;
            if (cp == ' ') {
                last_space = p;
                count_at_space = count;
            }
        }
        if (*p && *p != '\n' && last_space != nullptr) {
            p = last_space;  // break after the last space
            count = count_at_space;
        }
        lines.push_back({static_cast<size_t>(line_start - base), count});
        s = p;
        if (*s == '\n') {
            s++;
        }
    }
}

void sendResult(const char* button) {
    cJSON* f = cJSON_CreateObject();
    if (button != nullptr) {
        cJSON_AddStringToObject(f, "button", button);
    } else {
        cJSON_AddBoolToObject(f, "timeout", true);
    }
    events::emit("modal.result", f);
}

// ---- methods ----

cJSON* get(const cJSON*, rpc::Error*) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    State s = state;
    xSemaphoreGive(mutex);
    if (!s.visible) {
        return cJSON_CreateNull();
    }
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "title", s.title.c_str());
    cJSON_AddStringToObject(obj, "body", s.body.c_str());
    cJSON_AddStringToObject(obj, "buttons", buttonsName(s.buttons));
    cJSON_AddNumberToObject(obj, "timeout_ms", s.timeout_ms);
    if (s.timeout_ms != 0) {
        int64_t left = s.shown_ms + s.timeout_ms - esp_timer_get_time() / 1000;
        cJSON_AddNumberToObject(obj, "remaining_ms", static_cast<double>(left > 0 ? left : 0));
    }
    return obj;
}

cJSON* show(const cJSON* params, rpc::Error* err) {
    bool ok;
    const char* title = rpc::getOptString(params, "title", err, &ok);
    if (!ok) return nullptr;
    const char* body = rpc::getOptString(params, "body", err, &ok);
    if (!ok) return nullptr;
    const char* buttons = rpc::getOptString(params, "buttons", err, &ok);
    if (!ok) return nullptr;
    int timeout = 0;
    if (!rpc::getOptInt(params, "timeout_ms", 0, 3600000, &timeout, err)) {
        return nullptr;
    }
    Buttons b = Buttons::None;
    if (buttons != nullptr) {
        if (strcmp(buttons, "ok") == 0) {
            b = Buttons::Ok;
        } else if (strcmp(buttons, "yes_no") == 0) {
            b = Buttons::YesNo;
        } else if (strcmp(buttons, "none") != 0) {
            return rpc::fail(err, rpc::kInvalidParams, "buttons: none / ok / yes_no");
        }
    }
    if (title == nullptr && body == nullptr) {
        return rpc::fail(err, rpc::kInvalidParams, "title or body required");
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    state.visible = true;
    state.title = title ? title : "";
    state.body = body ? body : "";
    state.buttons = b;
    state.timeout_ms = static_cast<uint32_t>(timeout);
    state.shown_ms = esp_timer_get_time() / 1000;
    xSemaphoreGive(mutex);
    return cJSON_CreateObject();
}

cJSON* close(const cJSON*, rpc::Error*) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    state.visible = false;
    xSemaphoreGive(mutex);
    return cJSON_CreateObject();
}

const rpc::Method kMethods[] = {
    {"modal.get", get, nullptr},
    {"modal.show", show, nullptr},
    {"modal.close", close, nullptr},
};

}  // namespace

void registerMethods() {
    rpc::registerMethods(kMethods, sizeof(kMethods) / sizeof(kMethods[0]));
}

bool update(int64_t now_ms) {
    bool timed_out = false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (state.visible && state.timeout_ms != 0 && now_ms >= state.shown_ms + state.timeout_ms) {
        state.visible = false;
        timed_out = true;
    }
    frame = state;
    xSemaphoreGive(mutex);
    if (timed_out) {
        sendResult(nullptr);
    }
    if (frame.visible && frame.body != wrapped_body) {
        wrapped_body = frame.body;
        wrap(wrapped_body);
    }
    return frame.visible;
}

void render(const gfx::Band& b) {
    if (!frame.visible) {
        return;
    }
    gfx::fillRect(b, kX, kY, kW, kH, kBoxBg);
    gfx::strokeRect(b, kX, kY, kW, kH, 2, kBorder);
    int tw = gfx::textWidth(frame.title.c_str());
    gfx::drawText(b, kX + (kW - tw) / 2, kTitleY, frame.title.c_str(), kText, true);

    int max_lines = (kH - (kBodyY - kY) - kPad - (frame.buttons == Buttons::None ? 0 : kButtonH + 6)) / kLineH;
    for (int i = 0; i < static_cast<int>(lines.size()) && i < max_lines; i++) {
        gfx::drawTextN(b, kX + kPad, kBodyY + i * kLineH, wrapped_body.c_str() + lines[i].start, lines[i].count,
                       kText);
    }

    ButtonRect rects[2];
    int n = buttonRects(frame.buttons, rects);
    for (int i = 0; i < n; i++) {
        const ButtonRect& r = rects[i];
        gfx::fillRoundRect(b, r.x + r.w / 2.0f, r.y + r.h / 2.0f, r.w, r.h, 8, 0, r.bg);
        int lw = gfx::textWidth(r.label);
        gfx::drawText(b, r.x + (r.w - lw) / 2, r.y + (r.h - gfx::kFontHeight) / 2, r.label, kText, true);
    }
}

bool handleTap(int x, int y) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (!state.visible) {
        xSemaphoreGive(mutex);
        return false;
    }
    ButtonRect rects[2];
    int n = buttonRects(state.buttons, rects);
    const char* result = nullptr;
    for (int i = 0; i < n; i++) {
        if (x >= rects[i].x && x < rects[i].x + rects[i].w && y >= rects[i].y && y < rects[i].y + rects[i].h) {
            result = rects[i].result;
        }
    }
    if (result != nullptr) {
        state.visible = false;
    }
    xSemaphoreGive(mutex);
    if (result != nullptr) {
        sendResult(result);
    }
    return true;  // taps inside a visible modal never reach the face
}

}  // namespace modal
