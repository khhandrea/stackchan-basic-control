// StackChan general firmware: standalone StackChan controlled from a host over USB serial.

#include "esp_log.h"
#include "board.h"
#include "display.h"
#include "events.h"
#include "i2c_bus.h"
#include "imu.h"
#include "led.h"
#include "light.h"
#include "link.h"
#include "methods.h"
#include "methods_sys.h"
#include "notify.h"
#include "poller.h"
#include "power.h"
#include "rpc.h"
#include "rtc.h"
#include "streams.h"
#include "touch.h"

static const char* TAG = "main";

extern "C" void app_main() {
    hostlink::init();
    notify::installLogHook();

    // Hardware
    i2c::init();
    power::init();
    board::init();
    display::init();
    touch::init();
    imu::init();
    light::init();
    rtc::init();
    led::init();

    // Methods
    sys::registerMethods();
    events::registerMethods();
    streams::registerMethods();
    power::registerMethods();
    display::registerMethods();
    touch::registerMethods();
    imu::registerMethods();
    light::registerMethods();
    rtc::registerMethods();
    led::registerMethods();
    registerDebugMethods();

    // Background polling and streams (all sources registered before poller::start)
    streams::start();
    power::start();
    touch::start();
    imu::start();
    light::start();
    led::start();
    poller::start();
    display::start();

    sys::sendBootEvent();
    ESP_LOGI(TAG, "ready");

    rpc::serve();
}
