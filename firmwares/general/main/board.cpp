#include "board.h"

#include "esp_log.h"
#include "events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace board {
namespace {

const char* TAG = "board";

i2c::Device aw9523;
i2c::Device touch;
Panel detected = Panel::Ili9342c;

void delayMs(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

// FT6336 version: cipher 0xA3, firm id 0xA6, vendor 0xA8. ILI9342E units report firm 0x12 / vendor 0x11.
void detectPanel() {
    for (int attempt = 0; attempt < 5; attempt++) {
        const uint8_t work_mode[] = {0x00, 0x00};
        uint8_t info[6] = {};
        if (touch.transmit(work_mode, sizeof(work_mode)) == ESP_OK && touch.read(0xA3, info, sizeof(info)) == ESP_OK &&
            info[0] != 0 && info[5] != 0) {
            detected = (info[3] == 0x12 && info[5] == 0x11) ? Panel::Ili9342e : Panel::Ili9342c;
            ESP_LOGI(TAG, "FT6336 cipher 0x%02x firm 0x%02x vendor 0x%02x, panel %s", info[0], info[3], info[5],
                     detected == Panel::Ili9342e ? "ILI9342E" : "ILI9342C");
            return;
        }
        delayMs(20);
    }
    ESP_LOGW(TAG, "FT6336 version read failed, panel ILI9342C");
}

}  // namespace

void init() {
    if (aw9523.open(0x58)) {
        // Same setup as the official firmware (M5StackCoreS3Board::Aw9523)
        aw9523.write8(0x02, 0b00000111);  // P0 output levels
        aw9523.write8(0x03, 0b10001111);  // P1 output levels
        aw9523.write8(0x04, 0b00011000);  // P0 direction (1 = input)
        aw9523.write8(0x05, 0b00001100);  // P1 direction
        aw9523.write8(0x11, 0b00010000);  // GCR: P0 push-pull
        aw9523.write8(0x12, 0b11111111);  // P0 GPIO mode
        aw9523.write8(0x13, 0b11111111);  // P1 GPIO mode
        delayMs(50);
    } else {
        ESP_LOGE(TAG, "AW9523 not found");
        events::error("sensor_init", "aw9523");
    }
    if (touch.open(0x38)) {
        detectPanel();
    } else {
        ESP_LOGE(TAG, "FT6336 not found");
        events::error("sensor_init", "ft6336");
    }
}

void resetLcd() {
    aw9523.write8(0x03, 0b10000001);
    delayMs(20);
    aw9523.write8(0x03, 0b10000011);
    delayMs(10);
}

Panel panel() {
    return detected;
}

i2c::Device& ft6336() {
    return touch;
}

}  // namespace board
