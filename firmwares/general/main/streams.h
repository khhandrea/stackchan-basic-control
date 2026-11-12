// notify.stream: periodic values at a requested rate (stream.start / stream.stop).
#pragma once

#include "cJSON.h"

namespace streams {

// Fills obj with the stream's fields (same as the matching read method).
// Returns false if the source is not available.
using Source = bool (*)(cJSON* obj);

// Registers a source by name. Call before start().
void add(const char* name, Source source);

void registerMethods();
// Adds the stream tick to the poller.
void start();

}  // namespace streams
