#include "react.h"

#include <cstdint>

#include "esp_log.h"
#include "esp_timer.h"
#include "face.h"
#include "imu.h"
#include "poller.h"
#include "power.h"
#include "servo.h"
#include "touch.h"

namespace react {
namespace {

const char* TAG = "react";

constexpr int kPleasedHoldMs = 5000;  // after the last head touch
constexpr int kUpsetStillMs = 5000;
constexpr int kCuriousStillMs = 3000;
constexpr int kServoSettleMs = 500;  // IMU motion right after our own servo move still counts as ours

using face::Reaction;

Reaction current = Reaction::None;
int64_t last_touch_ms = 0;
int64_t still_since = 0;     // 0 = not still
int64_t servo_until = 0;     // our own servo motion (plus settling) until then
uint32_t last_shake = 0;

void set(Reaction r) {
    if (r == current) return;
    if (current == Reaction::Pleased) servo::reactLift(false);
    current = r;
    face::setReaction(r);
    if (r == Reaction::Pleased) servo::reactLift(true);
    static const char* const kNames[] = {"none", "pleased", "upset", "curious"};
    ESP_LOGI(TAG, "%s", kNames[static_cast<int>(r)]);
}

void tick() {
    int64_t now = esp_timer_get_time() / 1000;
    uint32_t shakes = imu::shakeCount();
    bool shaken = shakes != last_shake;
    last_shake = shakes;
    bool button = power::takeButtonPress();
    if (!face::reactEnabled()) {
        set(Reaction::None);
        return;
    }

    // IMU "moving", except motion caused by our own servos
    if (servo::moving()) servo_until = now + kServoSettleMs;
    bool moved = imu::motion() == imu::Motion::Moving && now >= servo_until;
    if (moved) {
        still_since = 0;
    } else if (still_since == 0) {
        still_since = now;
    }

    if (touch::headTouched()) {
        last_touch_ms = now;
        set(Reaction::Pleased);
        return;
    }
    if (shaken) {
        still_since = 0;
        set(Reaction::Upset);
        return;
    }
    if ((moved || button) && current != Reaction::Upset && current != Reaction::Pleased) {
        if (button) still_since = now;  // 3 s from the press unless moved meanwhile
        set(Reaction::Curious);
        return;
    }
    bool still_for_upset = still_since != 0 && now - still_since >= kUpsetStillMs;
    bool still_for_curious = still_since != 0 && now - still_since >= kCuriousStillMs;
    if ((current == Reaction::Pleased && now - last_touch_ms >= kPleasedHoldMs) ||
        (current == Reaction::Upset && still_for_upset) || (current == Reaction::Curious && still_for_curious)) {
        set(Reaction::None);
    }
}

}  // namespace

void start() {
    last_shake = imu::shakeCount();
    power::takeButtonPress();
    poller::add("react", tick, 50);
}

}  // namespace react
