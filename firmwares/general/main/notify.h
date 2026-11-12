// Device → host messages: protocol JSON output, notify.event and notify.log.
#pragma once

#include "cJSON.h"

namespace notify {

// Serializes msg as one line, sends it and frees msg.
void sendMessage(cJSON* msg);

// Sends notify.event with seq, ts_ms and name, followed by the items of fields.
// Takes ownership of fields (may be nullptr).
void event(const char* name, cJSON* fields);

// Routes ESP_LOG output to notify.log.
void installLogHook();

}  // namespace notify
