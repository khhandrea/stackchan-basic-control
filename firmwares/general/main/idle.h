// Idle: steps the device down when nobody interacts with it, and wakes it on any interaction.
// Interaction = a state-changing RPC (not reads, pings, streams or subscriptions), a screen or head
// touch, or the power button. Stages: 10 min sleepy face, 30 min backlight dimmed to 64,
// 60 min power save (screen off, LED 0 blinking red). Waking restores the previous settings.
// Methods: idle.get, idle.set_level (jump to a stage; passive, so it does not wake).
#pragma once

#include "cJSON.h"

namespace idle {

void registerMethods();
void start();
// Records an interaction and wakes the device immediately.
void activity();
// For each request: counts it as an interaction unless the method is passive.
void onRequest(const char* method);
// Adds idle_s and idle_level to a status object.
void addStatus(cJSON* obj);

}  // namespace idle
