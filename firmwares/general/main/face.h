// Face: expression, parameters, decorators, auto blink / gaze / reactions.
// Methods: face.get, face.set_expression, face.set_params, face.set_decorator, face.set_auto.
#pragma once

#include "gfx.h"

namespace face {

void registerMethods();

// Called by the display task once per frame: advances animation and captures the state to draw.
void update(int64_t now_ms);
void render(const gfx::Band& b);
// Idle override: draw the sleepy expression without changing the set one.
void setIdleSleepy(bool on);

// Reaction overlay drawn on top of the set face (react module); None clears it.
enum class Reaction { None, Pleased, Upset, Curious };  // happy + shy (mouth a little open), unhappy + sweat, neutral + question
void setReaction(Reaction r);
// face.set_auto "react" flag
bool reactEnabled();

}  // namespace face
