// Debug methods for hardware bring-up: raw I2C access (not part of the stable protocol).
#include "i2c_bus.h"
#include "methods.h"
#include "rpc.h"

namespace {

cJSON* scan(const cJSON*, rpc::Error*) {
    cJSON* result = cJSON_CreateArray();
    for (int addr = 0x08; addr < 0x78; addr++) {
        if (i2c::probe(static_cast<uint8_t>(addr))) {
            char hex[5];
            snprintf(hex, sizeof(hex), "0x%02x", addr);
            cJSON_AddItemToArray(result, cJSON_CreateString(hex));
        }
    }
    return result;
}

bool openDevice(const cJSON* params, i2c::Device* dev, rpc::Error* err) {
    int addr = 0;
    if (!rpc::getInt(params, "addr", 0x08, 0x77, &addr, err)) {
        return false;
    }
    if (!dev->open(static_cast<uint8_t>(addr))) {
        rpc::fail(err, rpc::kHardwareError, "no ack");
        return false;
    }
    return true;
}

void closeDevice(i2c::Device* dev) {
    if (dev->isOpen()) {
        i2c_master_bus_rm_device(dev->handle());
    }
}

cJSON* read(const cJSON* params, rpc::Error* err) {
    int reg = 0;
    int len = 1;
    if (!rpc::getInt(params, "reg", 0, 255, &reg, err) || !rpc::getOptInt(params, "len", 1, 32, &len, err)) {
        return nullptr;
    }
    i2c::Device dev;
    if (!openDevice(params, &dev, err)) {
        return nullptr;
    }
    uint8_t data[32];
    esp_err_t e = dev.read(static_cast<uint8_t>(reg), data, static_cast<size_t>(len));
    closeDevice(&dev);
    if (e != ESP_OK) {
        return rpc::fail(err, rpc::kHardwareError, esp_err_to_name(e));
    }
    cJSON* result = cJSON_CreateObject();
    cJSON* bytes = cJSON_AddArrayToObject(result, "data");
    for (int i = 0; i < len; i++) {
        cJSON_AddItemToArray(bytes, cJSON_CreateNumber(data[i]));
    }
    return result;
}

cJSON* write(const cJSON* params, rpc::Error* err) {
    int reg = 0;
    if (!rpc::getInt(params, "reg", 0, 255, &reg, err)) {
        return nullptr;
    }
    const cJSON* list = cJSON_GetObjectItemCaseSensitive(params, "data");
    int len = cJSON_GetArraySize(list);
    if (!cJSON_IsArray(list) || len < 1 || len > 32) {
        return rpc::fail(err, rpc::kInvalidParams, "data: array of 1-32 bytes");
    }
    uint8_t data[32];
    for (int i = 0; i < len; i++) {
        const cJSON* b = cJSON_GetArrayItem(list, i);
        if (!cJSON_IsNumber(b) || b->valueint < 0 || b->valueint > 255) {
            return rpc::fail(err, rpc::kInvalidParams, "data: bytes 0-255");
        }
        data[i] = static_cast<uint8_t>(b->valueint);
    }
    i2c::Device dev;
    if (!openDevice(params, &dev, err)) {
        return nullptr;
    }
    esp_err_t e = dev.write(static_cast<uint8_t>(reg), data, static_cast<size_t>(len));
    closeDevice(&dev);
    if (e != ESP_OK) {
        return rpc::fail(err, rpc::kHardwareError, esp_err_to_name(e));
    }
    return cJSON_CreateObject();
}

const rpc::Method kMethods[] = {
    {"i2c.scan", scan, nullptr},
    {"i2c.read", read, nullptr},
    {"i2c.write", write, nullptr},
};

}  // namespace

void registerDebugMethods() {
    rpc::registerMethods(kMethods, sizeof(kMethods) / sizeof(kMethods[0]));
}
