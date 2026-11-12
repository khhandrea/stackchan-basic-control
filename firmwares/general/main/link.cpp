#include "link.h"

#include <cstring>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace hostlink {
namespace {

constexpr uint32_t kTxBufferSize = 4096;
constexpr uint32_t kRxBufferSize = 1024;
constexpr TickType_t kWriteTimeout = pdMS_TO_TICKS(50);

SemaphoreHandle_t tx_mutex = nullptr;

void writeAll(const char* data, size_t len) {
    while (len > 0) {
        int n = usb_serial_jtag_write_bytes(data, len, kWriteTimeout);
        if (n <= 0) {
            return;  // host stopped reading; drop the rest
        }
        data += n;
        len -= static_cast<size_t>(n);
    }
}

}  // namespace

void init() {
    usb_serial_jtag_driver_config_t config = {
        .tx_buffer_size = kTxBufferSize,
        .rx_buffer_size = kRxBufferSize,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));
    // Route stdout (printf, panic handler excluded) through the same driver.
    usb_serial_jtag_vfs_use_driver();
    tx_mutex = xSemaphoreCreateMutex();
}

ReadResult readLine(char* buf, size_t cap) {
    size_t len = 0;
    bool overflow = false;
    while (true) {
        char c;
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c == '\n') {
            if (overflow) {
                return ReadResult::TooLong;
            }
            if (len > 0 && buf[len - 1] == '\r') {
                len--;
            }
            buf[len] = '\0';
            return ReadResult::Line;
        }
        if (overflow) {
            continue;
        }
        if (len + 1 >= cap) {
            overflow = true;
            continue;
        }
        buf[len++] = c;
    }
}

void send(const char* line, size_t len) {
    if (tx_mutex == nullptr || !usb_serial_jtag_is_connected()) {
        return;
    }
    xSemaphoreTake(tx_mutex, portMAX_DELAY);
    writeAll(line, len);
    writeAll("\n", 1);
    xSemaphoreGive(tx_mutex);
}

void flush() {
    usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(200));
}

}  // namespace hostlink
