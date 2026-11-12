// Screen touch (FT6336) and head touch (Si12T, 3 zones). Method: touch.get. Events: touch.screen, touch.head.
#pragma once

namespace touch {

void init();
void registerMethods();
// Registers polling. Call after init().
void start();

}  // namespace touch
