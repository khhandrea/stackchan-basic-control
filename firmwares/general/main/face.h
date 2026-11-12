// Face: expression, parameters, decorators, auto blink / breath.
// Methods: face.get, face.set_expression, face.set_params, face.set_decorator, face.set_auto.
#pragma once

#include "gfx.h"

namespace face {

void registerMethods();

// Called by the display task once per frame: advances animation and captures the state to draw.
void update(int64_t now_ms);
void render(const gfx::Band& b);

}  // namespace face
