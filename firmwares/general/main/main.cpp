// StackChan general firmware: standalone StackChan controlled from a host over USB serial.

#include "esp_log.h"
#include "link.h"
#include "methods_sys.h"
#include "notify.h"
#include "rpc.h"

static const char* TAG = "main";

extern "C" void app_main() {
    hostlink::init();
    notify::installLogHook();

    sys::registerMethods();

    sys::sendBootEvent();
    ESP_LOGI(TAG, "ready");

    rpc::serve();
}
