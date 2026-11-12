// USB-Serial/JTAG line link to the host: one JSON object per line.
#pragma once

#include <cstddef>

namespace hostlink {

constexpr size_t kMaxRequestLine = 1024;

void init();

enum class ReadResult { Line, TooLong };

// Blocks until a full line arrives. The line is NUL-terminated without '\n'.
// A line longer than kMaxRequestLine is discarded up to its '\n' and reported as TooLong.
ReadResult readLine(char* buf, size_t cap);

// Sends one line (a '\n' is appended). Thread-safe; lines are never interleaved.
// Dropped if no host is connected. Must not call ESP_LOG (log output is routed here).
void send(const char* line, size_t len);

// Waits until queued output has been sent (used before reboot).
void flush();

}  // namespace hostlink
