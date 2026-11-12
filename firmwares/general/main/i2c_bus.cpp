#include "i2c_bus.h"

#include <cstdlib>
#include <cstring>

#include "esp_check.h"

namespace i2c {
namespace {

constexpr int kTimeoutMs = 50;
i2c_master_bus_handle_t bus_handle = nullptr;

}  // namespace

void init() {
    i2c_master_bus_config_t config = {};
    config.i2c_port = I2C_NUM_1;
    config.sda_io_num = GPIO_NUM_12;
    config.scl_io_num = GPIO_NUM_11;
    config.clk_source = I2C_CLK_SRC_DEFAULT;
    config.glitch_ignore_cnt = 7;
    config.flags.enable_internal_pullup = 1;
    ESP_ERROR_CHECK(i2c_new_master_bus(&config, &bus_handle));
}

i2c_master_bus_handle_t bus() {
    return bus_handle;
}

bool probe(uint8_t addr) {
    return i2c_master_probe(bus_handle, addr, kTimeoutMs) == ESP_OK;
}

bool Device::open(uint8_t addr, uint32_t speed_hz) {
    if (!probe(addr)) {
        return false;
    }
    i2c_device_config_t config = {};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = addr;
    config.scl_speed_hz = speed_hz;
    if (i2c_master_bus_add_device(bus_handle, &config, &dev_) != ESP_OK) {
        dev_ = nullptr;
        return false;
    }
    addr_ = addr;
    return true;
}

esp_err_t Device::read(uint8_t reg, uint8_t* data, size_t len) {
    if (dev_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(dev_, &reg, 1, data, len, kTimeoutMs);
}

esp_err_t Device::write(uint8_t reg, const uint8_t* data, size_t len) {
    if (dev_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    // Register byte and data in one buffer: on the stack for short writes, heap otherwise
    uint8_t small[33];
    uint8_t* buf = len + 1 <= sizeof(small) ? small : static_cast<uint8_t*>(malloc(len + 1));
    if (buf == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    buf[0] = reg;
    memcpy(buf + 1, data, len);
    esp_err_t err = i2c_master_transmit(dev_, buf, len + 1, kTimeoutMs);
    if (buf != small) {
        free(buf);
    }
    return err;
}

esp_err_t Device::update8(uint8_t reg, uint8_t mask, uint8_t bits) {
    uint8_t value;
    ESP_RETURN_ON_ERROR(read8(reg, &value), "i2c", "read 0x%02x", reg);
    return write8(reg, static_cast<uint8_t>((value & ~mask) | (bits & mask)));
}

esp_err_t Device::transmit(const uint8_t* data, size_t len) {
    return dev_ == nullptr ? ESP_ERR_INVALID_STATE : i2c_master_transmit(dev_, data, len, kTimeoutMs);
}

esp_err_t Device::receive(uint8_t* data, size_t len) {
    return dev_ == nullptr ? ESP_ERR_INVALID_STATE : i2c_master_receive(dev_, data, len, kTimeoutMs);
}

}  // namespace i2c
