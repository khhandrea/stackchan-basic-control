// notify.event subscriptions (sys.subscribe / sys.unsubscribe).
#pragma once

#include "cJSON.h"

namespace events {

void registerMethods();

// Sends the event if subscribed (boot and error are always sent). Takes ownership of fields.
void emit(const char* name, cJSON* fields);

// Error event helper: kind = sensor_init / i2c / low_memory.
void error(const char* kind, const char* detail);

}  // namespace events
