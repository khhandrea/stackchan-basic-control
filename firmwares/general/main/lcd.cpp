#include "lcd.h"

#include "board.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace lcd {
namespace {

const char* TAG = "lcd";

esp_lcd_panel_io_handle_t io = nullptr;
esp_lcd_panel_handle_t panel = nullptr;
SemaphoreHandle_t free_bands = nullptr;
uint16_t* bands[2] = {};
int next_band = 0;

struct InitCommand {
    uint8_t command;
    uint8_t data[15];
    uint8_t size;
    uint16_t delay_ms;
};

// ILI9342E panel init from the official firmware (units from 2026-08)
constexpr InitCommand kIli9342eInit[] = {
    {0xDD, {0x01}, 1, 0},
    {0x3A, {0x55}, 1, 0},
    {0x21, {}, 0, 0},
    {0x36, {0x08}, 1, 0},
    {0xD5, {0x00}, 1, 0},
    {0xB1, {0x22}, 1, 0},
    {0xC8, {0x38}, 1, 0},
    {0xCB, {0x1C}, 1, 0},
    {0xC9, {0x1A}, 1, 0},
    {0xCA, {0x1A}, 1, 0},
    {0xB7, {0x5A, 0x41, 0x11, 0x19}, 4, 0},
    {0xE4, {0x04, 0x08, 0x11, 0x06, 0x12, 0x07, 0x3A, 0x76, 0x47, 0x07, 0x0F, 0x0A, 0x11, 0x19, 0x05}, 15, 0},
    {0xE5, {0x02, 0x03, 0x07, 0x06, 0x12, 0x07, 0x36, 0x5F, 0x48, 0x06, 0x10, 0x0C, 0x16, 0x14, 0x09}, 15, 0},
    {0x11, {}, 0, 120},
    {0x29, {}, 0, 120},
};

bool onTransferDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(free_bands, &woken);
    return woken == pdTRUE;
}

}  // namespace

bool init() {
    spi_bus_config_t bus = {};
    bus.mosi_io_num = GPIO_NUM_37;
    bus.miso_io_num = GPIO_NUM_NC;
    bus.sclk_io_num = GPIO_NUM_36;
    bus.quadwp_io_num = GPIO_NUM_NC;
    bus.quadhd_io_num = GPIO_NUM_NC;
    bus.max_transfer_sz = kWidth * kBandHeight * sizeof(uint16_t);
    if (spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        events::error("sensor_init", "lcd spi");
        return false;
    }

    free_bands = xSemaphoreCreateCounting(2, 2);
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = GPIO_NUM_3;
    io_config.dc_gpio_num = GPIO_NUM_35;
    io_config.spi_mode = 2;
    io_config.pclk_hz = 40 * 1000 * 1000;
    io_config.trans_queue_depth = 4;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.on_color_trans_done = onTransferDone;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &io));

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = GPIO_NUM_NC;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    panel_config.bits_per_pixel = 16;
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &panel_config, &panel));

    esp_lcd_panel_reset(panel);
    board::resetLcd();
    esp_lcd_panel_init(panel);
    if (board::panel() == board::Panel::Ili9342e) {
        ESP_LOGI(TAG, "apply ILI9342E init");
        for (const InitCommand& c : kIli9342eInit) {
            esp_lcd_panel_io_tx_param(io, c.command, c.size ? c.data : nullptr, c.size);
            if (c.delay_ms) {
                vTaskDelay(pdMS_TO_TICKS(c.delay_ms));
            }
        }
    }
    esp_lcd_panel_invert_color(panel, true);
    esp_lcd_panel_swap_xy(panel, false);
    esp_lcd_panel_mirror(panel, false, false);

    for (auto& b : bands) {
        b = static_cast<uint16_t*>(
            heap_caps_malloc(kWidth * kBandHeight * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        if (b == nullptr) {
            events::error("low_memory", "lcd band buffer");
            return false;
        }
    }
    return true;
}

bool ready() {
    return panel != nullptr && bands[1] != nullptr;
}

uint16_t* acquireBand() {
    xSemaphoreTake(free_bands, portMAX_DELAY);
    uint16_t* b = bands[next_band];
    next_band ^= 1;
    return b;
}

void pushBand(uint16_t* band, int y0, int rows) {
    esp_lcd_panel_draw_bitmap(panel, 0, y0, kWidth, y0 + rows, band);
}

void setOn(bool on) {
    if (panel != nullptr) {
        esp_lcd_panel_disp_on_off(panel, on);
    }
}

}  // namespace lcd
