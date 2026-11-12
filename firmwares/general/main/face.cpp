#include "face.h"

#include <cmath>
#include <cstring>

#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "rpc.h"

namespace face {
namespace {

enum Expression { Neutral, Happy, Angry, Sad, Doubt, Sleepy, kExpressionCount };
constexpr const char* kExpressionNames[] = {"neutral", "happy", "angry", "sad", "doubt", "sleepy"};

enum Decorator { Heart, AngryMark, Sweat, Dizzy, Shy, kDecoratorCount };
constexpr const char* kDecoratorNames[] = {"heart", "angry", "sweat", "dizzy", "shy"};

struct Params {
    float eye_x = 0;      // gaze -1 (left) .. 1 (right)
    float eye_y = 0;      // gaze -1 (up) .. 1 (down)
    float eye_open = 1;   // 0 .. 1
    float eye_rot = 0;    // degrees
    float eye_size = 1;   // 0.5 .. 1.5
    float mouth_open = 0; // 0 .. 1
    float mouth_rot = 0;  // degrees
};

struct State {
    Expression expression = Neutral;
    Params params;
    bool decorators[kDecoratorCount] = {};
    bool blink = true;
    bool breath = true;
};

SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
State state;   // guarded by mutex
State frame;   // copy used by the display task
float blink_open = 1;
float breath_dy = 0;
int64_t next_blink_ms = 3000;
int64_t blink_start_ms = -1;

constexpr uint16_t kBg = gfx::rgb(0, 0, 0);
constexpr uint16_t kFg = gfx::rgb(255, 255, 255);
constexpr uint16_t kPink = gfx::rgb(255, 110, 150);
constexpr uint16_t kBlush = gfx::rgb(230, 90, 120);
constexpr uint16_t kRed = gfx::rgb(240, 40, 40);
constexpr uint16_t kBlue = gfx::rgb(110, 190, 255);

constexpr float kEyeRadius = 16;
constexpr float kEyeSpacing = 60;  // from the center
constexpr float kEyeY = 105;
constexpr float kMouthY = 165;
constexpr int kBlinkMs = 180;

// Eye lid cut: pixels above the top line / below the bottom line are removed.
// Lines pass through (cx, cy + offset * r) with the given slope; slope > 0 lowers the inner side.
struct Lids {
    bool top = false;
    float top_offset = 0;
    float top_slope = 0;
    bool bottom = false;
    float bottom_offset = 0;
    bool crescent = false;  // happy: cut an ellipse below the center, leaving an upper arc
};

Lids lidsFor(Expression e, bool left) {
    Lids l;
    switch (e) {
        case Happy:
            l.crescent = true;
            break;
        case Angry:
            l.top = true;
            l.top_offset = -0.35f;
            l.top_slope = 0.55f;
            break;
        case Sad:
            l.top = true;
            l.top_offset = -0.35f;
            l.top_slope = -0.55f;
            break;
        case Doubt:
            if (left) {
                l.top = true;
                l.top_offset = 0.0f;
            }
            break;
        case Sleepy:
            l.top = true;
            l.top_offset = 0.15f;
            break;
        default:
            break;
    }
    return l;
}

void drawEye(const gfx::Band& b, const State& s, bool left) {
    const Params& p = s.params;
    float r = kEyeRadius * p.eye_size;
    float cx = 160 + (left ? -kEyeSpacing : kEyeSpacing) + p.eye_x * 12;
    float cy = kEyeY + p.eye_y * 10 + breath_dy;

    if (s.decorators[Dizzy]) {
        gfx::fillRing(b, cx, cy, r, r * 0.72f, kFg);
        gfx::fillRing(b, cx, cy, r * 0.48f, r * 0.24f, kFg);
        return;
    }

    float open = p.eye_open * blink_open;
    float ry = r * open;
    if (ry < 1.5f) {
        // closed: a short line
        gfx::fillRoundRect(b, cx, cy, r * 2, 3, 1.5f, left ? p.eye_rot : -p.eye_rot, kFg);
        return;
    }
    float angle = (left ? p.eye_rot : -p.eye_rot) * 3.14159265f / 180.0f;
    float c = cosf(angle);
    float sn = sinf(angle);
    Lids lids = lidsFor(s.expression, left);
    float inward = left ? 1.0f : -1.0f;  // +x toward the nose for the left eye

    gfx::fillShape(b, static_cast<int>(cx - r - 1), static_cast<int>(cy - r - 1), static_cast<int>(cx + r + 1),
                   static_cast<int>(cy + r + 1), kFg, [&](float x, float y) {
                       float dx = x - cx;
                       float dy = y - cy;
                       float u = (dx * c + dy * sn) / r;
                       float v = (-dx * sn + dy * c) / ry;
                       if (u * u + v * v > 1.0f) {
                           return false;
                       }
                       if (lids.top) {
                           float line = lids.top_offset * r + lids.top_slope * dx * inward;
                           if (dy < line) {
                               return false;
                           }
                       }
                       if (lids.bottom && dy > lids.bottom_offset * r) {
                           return false;
                       }
                       if (lids.crescent) {
                           float cu = dx / (r * 1.05f);
                           float cv = (dy - r * 0.55f) / (r * 0.95f);
                           if (cu * cu + cv * cv <= 1.0f) {
                               return false;
                           }
                       }
                       return true;
                   });
}

void drawMouth(const gfx::Band& b, const State& s) {
    const Params& p = s.params;
    float w = 56;
    float rot = p.mouth_rot;
    float open = p.mouth_open;
    switch (s.expression) {
        case Happy: w = 72; open = open < 0.25f ? 0.25f : open; break;
        case Angry: w = 44; break;
        case Sad: w = 40; break;
        case Doubt: w = 44; rot += 10; break;
        case Sleepy: w = 30; break;
        default: break;
    }
    float h = 5 + open * 36;
    gfx::fillRoundRect(b, 160, kMouthY + breath_dy, w, h, h < 10 ? h / 2 : 6, rot, kFg);
}

void drawDecorators(const gfx::Band& b, const State& s) {
    float dy = breath_dy;
    if (s.decorators[Shy]) {
        gfx::fillEllipse(b, 160 - kEyeSpacing - 8, kEyeY + 38 + dy, 22, 9, 0, kBlush);
        gfx::fillEllipse(b, 160 + kEyeSpacing + 8, kEyeY + 38 + dy, 22, 9, 0, kBlush);
    }
    if (s.decorators[Heart]) {
        gfx::fillHeart(b, 270, 45 + dy, 34, kPink);
    }
    if (s.decorators[AngryMark]) {
        // Four bent strokes around (55, 45), like the comic "vein" mark
        for (int i = 0; i < 4; i++) {
            float a = i * 90.0f + 45.0f;
            float rad = a * 3.14159265f / 180.0f;
            gfx::fillRoundRect(b, 55 + 11 * cosf(rad), 45 + dy + 11 * sinf(rad), 16, 5, 2.5f, a + 90, kRed);
        }
    }
    if (s.decorators[Sweat]) {
        gfx::fillDrop(b, 262, 80 + dy, 9, kBlue);
    }
}

void updateBlink(int64_t now) {
    if (!frame.blink) {
        blink_open = 1;
        blink_start_ms = -1;
        return;
    }
    if (blink_start_ms < 0 && now >= next_blink_ms) {
        blink_start_ms = now;
    }
    if (blink_start_ms >= 0) {
        int64_t t = now - blink_start_ms;
        if (t >= kBlinkMs) {
            blink_open = 1;
            blink_start_ms = -1;
            next_blink_ms = now + 2500 + esp_random() % 3500;
        } else {
            float half = kBlinkMs / 2.0f;
            blink_open = fabsf(t - half) / half;
        }
    }
}

// ---- methods ----

cJSON* stateJson() {
    xSemaphoreTake(mutex, portMAX_DELAY);
    State s = state;
    xSemaphoreGive(mutex);
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "expression", kExpressionNames[s.expression]);
    cJSON* p = cJSON_AddObjectToObject(obj, "params");
    cJSON_AddNumberToObject(p, "eye_x", s.params.eye_x);
    cJSON_AddNumberToObject(p, "eye_y", s.params.eye_y);
    cJSON_AddNumberToObject(p, "eye_open", s.params.eye_open);
    cJSON_AddNumberToObject(p, "eye_rot", s.params.eye_rot);
    cJSON_AddNumberToObject(p, "eye_size", s.params.eye_size);
    cJSON_AddNumberToObject(p, "mouth_open", s.params.mouth_open);
    cJSON_AddNumberToObject(p, "mouth_rot", s.params.mouth_rot);
    cJSON* d = cJSON_AddArrayToObject(obj, "decorators");
    for (int i = 0; i < kDecoratorCount; i++) {
        if (s.decorators[i]) {
            cJSON_AddItemToArray(d, cJSON_CreateString(kDecoratorNames[i]));
        }
    }
    cJSON* a = cJSON_AddObjectToObject(obj, "auto");
    cJSON_AddBoolToObject(a, "blink", s.blink);
    cJSON_AddBoolToObject(a, "breath", s.breath);
    return obj;
}

int lookup(const char* name, const char* const* names, int count) {
    for (int i = 0; i < count; i++) {
        if (strcmp(names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

cJSON* get(const cJSON*, rpc::Error*) {
    return stateJson();
}

cJSON* setExpression(const cJSON* params, rpc::Error* err) {
    const char* name = rpc::getString(params, "name", err);
    if (name == nullptr) {
        return nullptr;
    }
    int e = lookup(name, kExpressionNames, kExpressionCount);
    if (e < 0) {
        return rpc::fail(err, rpc::kInvalidParams, "name: neutral / happy / angry / sad / doubt / sleepy");
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    state.expression = static_cast<Expression>(e);
    xSemaphoreGive(mutex);
    return cJSON_CreateObject();
}

cJSON* setParams(const cJSON* params, rpc::Error* err) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    Params p = state.params;
    xSemaphoreGive(mutex);
    double v;
    struct Field {
        const char* name;
        float* target;
        double min, max;
    } fields[] = {
        {"eye_x", &p.eye_x, -1, 1},          {"eye_y", &p.eye_y, -1, 1},
        {"eye_open", &p.eye_open, 0, 1},     {"eye_rot", &p.eye_rot, -45, 45},
        {"eye_size", &p.eye_size, 0.5, 1.5}, {"mouth_open", &p.mouth_open, 0, 1},
        {"mouth_rot", &p.mouth_rot, -45, 45},
    };
    for (Field& f : fields) {
        v = *f.target;
        if (!rpc::getOptNumber(params, f.name, f.min, f.max, &v, err)) {
            return nullptr;
        }
        *f.target = static_cast<float>(v);
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    state.params = p;
    xSemaphoreGive(mutex);
    return cJSON_CreateObject();
}

cJSON* setDecorator(const cJSON* params, rpc::Error* err) {
    const char* name = rpc::getString(params, "name", err);
    bool on = false;
    if (name == nullptr || !rpc::getBool(params, "on", &on, err)) {
        return nullptr;
    }
    int d = lookup(name, kDecoratorNames, kDecoratorCount);
    if (d < 0) {
        return rpc::fail(err, rpc::kInvalidParams, "name: heart / angry / sweat / dizzy / shy");
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    state.decorators[d] = on;
    xSemaphoreGive(mutex);
    return cJSON_CreateObject();
}

cJSON* setAuto(const cJSON* params, rpc::Error* err) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool blink = state.blink;
    bool breath = state.breath;
    xSemaphoreGive(mutex);
    if (!rpc::getOptBool(params, "blink", &blink, err) || !rpc::getOptBool(params, "breath", &breath, err)) {
        return nullptr;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    state.blink = blink;
    state.breath = breath;
    xSemaphoreGive(mutex);
    return cJSON_CreateObject();
}

const rpc::Method kMethods[] = {
    {"face.get", get, nullptr},
    {"face.set_expression", setExpression, nullptr},
    {"face.set_params", setParams, nullptr},
    {"face.set_decorator", setDecorator, nullptr},
    {"face.set_auto", setAuto, nullptr},
};

}  // namespace

void registerMethods() {
    rpc::registerMethods(kMethods, sizeof(kMethods) / sizeof(kMethods[0]));
}

void update(int64_t now_ms) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    frame = state;
    xSemaphoreGive(mutex);
    updateBlink(now_ms);
    breath_dy = frame.breath ? 3.0f * sinf(2 * 3.14159265f * (now_ms % 4000) / 4000.0f) : 0;
}

void render(const gfx::Band& b) {
    gfx::fill(b, kBg);
    drawDecorators(b, frame);
    drawEye(b, frame, true);
    drawEye(b, frame, false);
    drawMouth(b, frame);
}

}  // namespace face
