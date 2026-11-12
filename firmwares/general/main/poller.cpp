#include "poller.h"

#include <vector>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace poller {
namespace {

constexpr uint32_t kTickMs = 10;

struct Entry {
    const char* name;
    void (*fn)();
    uint32_t period_ms;
    int64_t next_ms;
};

std::vector<Entry> entries;

void run(void*) {
    TickType_t last_wake = xTaskGetTickCount();
    while (true) {
        int64_t now = esp_timer_get_time() / 1000;
        for (Entry& e : entries) {
            if (now >= e.next_ms) {
                e.fn();
                e.next_ms += e.period_ms;
                if (e.next_ms <= now) {
                    e.next_ms = now + e.period_ms;  // fell behind; skip missed runs
                }
            }
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kTickMs));
    }
}

}  // namespace

void add(const char* name, void (*fn)(), uint32_t period_ms) {
    entries.push_back({name, fn, period_ms, esp_timer_get_time() / 1000 + period_ms});
}

void start() {
    xTaskCreatePinnedToCore(run, "poller", 8192, nullptr, 5, nullptr, 1);
}

}  // namespace poller
