// Internal I2C bus (SCL = G11, SDA = G12) and register access helpers.
#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/i2c_master.h"

namespace i2c {

void init();
i2c_master_bus_handle_t bus();
bool probe(uint8_t addr);

class Device {
public:
    Device() = default;
    // Adds the device to the bus. Returns false if it does not answer a probe.
    bool open(uint8_t addr, uint32_t speed_hz = 400000);
    bool isOpen() const { return dev_ != nullptr; }
    uint8_t addr() const { return addr_; }

    esp_err_t read(uint8_t reg, uint8_t* data, size_t len);
    esp_err_t write(uint8_t reg, const uint8_t* data, size_t len);
    esp_err_t read8(uint8_t reg, uint8_t* value) { return read(reg, value, 1); }
    esp_err_t write8(uint8_t reg, uint8_t value) { return write(reg, &value, 1); }
    // Read-modify-write: value = (value & ~mask) | (bits & mask)
    esp_err_t update8(uint8_t reg, uint8_t mask, uint8_t bits);
    // Raw transfers without a register byte.
    esp_err_t transmit(const uint8_t* data, size_t len);
    esp_err_t receive(uint8_t* data, size_t len);

    i2c_master_dev_handle_t handle() const { return dev_; }

private:
    i2c_master_dev_handle_t dev_ = nullptr;
    uint8_t addr_ = 0;
};

}  // namespace i2c
