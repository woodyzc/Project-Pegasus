#include "SensorBus.h"

#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {

constexpr int SDA_PIN = 11;
constexpr int SCL_PIN = 10;
constexpr uint32_t BUS_HZ = 400000;

SemaphoreHandle_t s_lock = nullptr;
bool s_begun = false;

struct Locked {
    Locked() {
        if (s_lock != nullptr) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
        }
    }
    ~Locked() {
        if (s_lock != nullptr) {
            xSemaphoreGive(s_lock);
        }
    }
    Locked(const Locked &) = delete;
    Locked &operator=(const Locked &) = delete;
};

} // namespace

void SensorBus_Begin() {
    if (s_lock == nullptr) {
        s_lock = xSemaphoreCreateMutex();
    }
    Locked guard;
    if (s_begun) {
        return;
    }
    Wire.begin(SDA_PIN, SCL_PIN, BUS_HZ);
    s_begun = true;
}

bool SensorBus_Probe(uint8_t addr) {
    Locked guard;
    Wire.beginTransmission(addr);
    return Wire.endTransmission(true) == 0;
}

bool SensorBus_Read(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len) {
    if (buf == nullptr || len == 0) {
        return false;
    }
    Locked guard;
    Wire.beginTransmission(addr);
    Wire.write(reg);
    // Repeated START, not a STOP. See the header.
    if (Wire.endTransmission(false) != 0) {
        return false;
    }
    if (Wire.requestFrom((int)addr, (int)len) != (int)len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)Wire.read();
    }
    return true;
}

bool SensorBus_Write(uint8_t addr, uint8_t reg, uint8_t value) {
    Locked guard;
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission(true) == 0;
}
