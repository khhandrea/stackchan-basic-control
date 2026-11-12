// StackChan general firmware: standalone StackChan controlled from a host over USB serial.

#include "esp_log.h"
#include "events.h"
#include "i2c_bus.h"
#include "link.h"
#include "methods.h"
#include "methods_sys.h"
#include "notify.h"
#include "poller.h"
#include "power.h"
#include "rpc.h"
#include "streams.h"

static const char* TAG = "main";

extern "C" void app_main() {
    hostlink::init();
    notify::installLogHook();

    // Hardware
    i2c::init();
    power::init();

    // Methods
    sys::registerMethods();
    events::registerMethods();
    streams::registerMethods();
    power::registerMethods();
    registerDebugMethods();

    // Background polling and streams (all sources registered before poller::start)
    streams::start();
    power::start();
    poller::start();

    sys::sendBootEvent();
    ESP_LOGI(TAG, "ready");

    rpc::serve();
}
