// One background task that runs registered periodic functions (sensor polling, streams).
#pragma once

#include <cstdint>

namespace poller {

// Registers fn to run every period_ms (multiple of 10 ms). Call before start().
void add(const char* name, void (*fn)(), uint32_t period_ms);
void start();

}  // namespace poller
