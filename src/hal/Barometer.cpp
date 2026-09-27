#include "Barometer.h"

#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "../system/BaroAltitude.h"

namespace {

// The sensor bus. Wire1 belongs to the touch controller (hal/Touch.cpp); this
// is the other one, and nothing else in the firmware drives it.
constexpr int SENSOR_I2C_SDA = 11;
constexpr int SENSOR_I2C_SCL = 10;
constexpr uint32_t SENSOR_I2C_HZ = 400000;

// Both addresses the breakout can present, high one first because that is
// what this board's module actually does with its pull-ups.
constexpr uint8_t ADDR_HIGH = 0x47;
constexpr uint8_t ADDR_LOW = 0x46;

// ---- Registers, from boschsensortec/BMP5_SensorAPI ----
constexpr uint8_t REG_CHIP_ID = 0x01;
constexpr uint8_t REG_TEMP_XLSB = 0x1D; // 0x1D..0x1F temp, 0x20..0x22 press
constexpr uint8_t REG_STATUS = 0x28;
constexpr uint8_t REG_OSR_CONFIG = 0x36;
constexpr uint8_t REG_ODR_CONFIG = 0x37;

constexpr uint8_t CHIP_ID_PRIM = 0x50;
constexpr uint8_t CHIP_ID_SEC = 0x51;

constexpr uint8_t STATUS_NVM_RDY = 0x02;

// OSR_CONFIG: bit 6 enables pressure, bits 3..5 are its oversampling, bits
// 0..2 the temperature's. Pressure measurement is OFF by default -- without
// bit 6 the part runs happily and reports temperature while the pressure
// registers stay at their reset value, which reads as a barometer stuck at a
// fixed altitude rather than as a configuration mistake.
constexpr uint8_t OSR_PRESS_EN = 0x40;
constexpr uint8_t OSR_PRESS_X4 = (0x02 << 3);
constexpr uint8_t OSR_TEMP_X2 = 0x01;

// ODR_CONFIG: power mode in bits 0..1, output data rate in bits 2..6.
// Normal mode is 1. ODR 0x1A is ~5Hz, which is far more often than a climb
// total needs and keeps the filter in BaroAltitude fed.
constexpr uint8_t PWR_NORMAL = 0x01;
constexpr uint8_t ODR_5HZ = (0x1A << 2);

// How hard the smoother works. At ~2Hz sampling this settles a step in a few
// seconds, which is slower than a rider climbs and much faster than weather
// moves.
constexpr float SMOOTH_ALPHA = 0.20f;

uint8_t s_addr = 0;
uint8_t s_chip_id = 0;
bool s_found = false;

BaroSmoother_t s_smoother;
volatile float s_altitude_m = 0.0f;
volatile bool s_have_altitude = false;

// The last good sample and when it arrived, for anyone who wants a number
// without touching the bus. The timestamp is not optional -- see
// Barometer_Reading() in the header.
volatile float s_last_pa = 0.0f;
volatile float s_last_c = 0.0f;
volatile uint32_t s_last_ms = 0;

// Six sample periods. Long enough that a single missed transfer does not
// flicker the panel, short enough that a wire falling out is visible while the
// board is still in your hands.
constexpr uint32_t STALE_MS = 3000;

// One owner at a time. Arduino's Wire is not thread-safe and this is the only
// module driving the sensor bus -- but "only module" is not "only task": the
// Core 0 sampler and anything calling from the UI are two, and two concurrent
// transactions corrupt each other.
//
// Added pre-emptively rather than after a failure, which is worth recording
// honestly: the "found but not reading" this went in alongside was a loose
// jumper, diagnosed as this race and blamed on the settings page before the
// wire was checked. The lock is still right -- the two callers existed and the
// bus has no arbitration -- but nothing here has ever been observed to
// corrupt, and the next person reading this should not go looking for the
// symptom that proved it.
SemaphoreHandle_t s_lock = nullptr;

struct BusLock {
    BusLock() {
        if (s_lock != nullptr) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
        }
    }
    ~BusLock() {
        if (s_lock != nullptr) {
            xSemaphoreGive(s_lock);
        }
    }
    BusLock(const BusLock &) = delete;
    BusLock &operator=(const BusLock &) = delete;
};

bool WriteReg(uint8_t reg, uint8_t value) {
    if (s_addr == 0) {
        return false;
    }
    Wire.beginTransmission(s_addr);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission(true) == 0;
}

bool ReadRegs(uint8_t reg, uint8_t *buf, uint8_t len) {
    if (s_addr == 0) {
        return false;
    }
    Wire.beginTransmission(s_addr);
    Wire.write(reg);
    // Repeated START, as Bosch's I2C read does. A STOP here would end the
    // transaction and leave the following read unattached to the register
    // just written -- the exact failure that made the CST3530 look dead for
    // most of a day (CLAUDE.md §2).
    if (Wire.endTransmission(false) != 0) {
        return false;
    }
    if (Wire.requestFrom((int)s_addr, (int)len) != (int)len) {
        return false;
    }
    for (uint8_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)Wire.read();
    }
    return true;
}

bool AddressAcks(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission(true) == 0;
}

void BaroTask(void *pv) {
    (void)pv;
    for (;;) {
        float pa = 0.0f;
        float c = 0.0f;
        if (Barometer_Read(&pa, &c)) {
            s_last_pa = pa;
            s_last_c = c;
            s_last_ms = millis();
            const float raw = BaroAltitude_MetresFromPa(pa);
            s_altitude_m = BaroSmoother_Push(&s_smoother, raw, SMOOTH_ALPHA);
            s_have_altitude = true;
        }
        // 500ms: twice the rate the panel can usefully show, and slow enough
        // that the bus is idle for the RTC and IMU whenever they are driven.
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

} // namespace

void Barometer_Init() {
    if (s_lock == nullptr) {
        s_lock = xSemaphoreCreateMutex();
    }
    Wire.begin(SENSOR_I2C_SDA, SENSOR_I2C_SCL, SENSOR_I2C_HZ);
    BaroSmoother_Reset(&s_smoother);

    if (AddressAcks(ADDR_HIGH)) {
        s_addr = ADDR_HIGH;
    } else if (AddressAcks(ADDR_LOW)) {
        s_addr = ADDR_LOW;
    } else {
        s_addr = 0;
        return;
    }

    // The chip ID is checked, unlike the touch controller's signature, and
    // the difference is worth stating: 0x47 is a plain I2C address that other
    // parts can and do use, so an ACK alone does not mean a barometer. The
    // CST3530 case was the opposite -- the address was unique to the part and
    // the "signature" belonged to a different chip entirely.
    if (!ReadRegs(REG_CHIP_ID, &s_chip_id, 1)) {
        s_addr = 0;
        return;
    }
    if (s_chip_id != CHIP_ID_PRIM && s_chip_id != CHIP_ID_SEC) {
        return; // something is there; it is not this sensor. Keep the id to show.
    }

    // Wait for the NVM copy to finish. Configuring through it leaves the
    // trim registers half-loaded and the readings quietly wrong.
    for (int i = 0; i < 20; i++) {
        uint8_t status = 0;
        if (ReadRegs(REG_STATUS, &status, 1) && (status & STATUS_NVM_RDY)) {
            break;
        }
        delay(5);
    }

    // Oversampling before power mode: the part latches this configuration
    // when it enters normal mode.
    if (!WriteReg(REG_OSR_CONFIG, OSR_PRESS_EN | OSR_PRESS_X4 | OSR_TEMP_X2)) {
        return;
    }
    if (!WriteReg(REG_ODR_CONFIG, ODR_5HZ | PWR_NORMAL)) {
        return;
    }

    s_found = true;
}

bool Barometer_Found() { return s_found; }
uint8_t Barometer_Address() { return s_addr; }
uint8_t Barometer_ChipId() { return s_chip_id; }

bool Barometer_Read(float *pressure_pa, float *temperature_c) {
    if (!s_found) {
        return false;
    }

    BusLock guard;

    // Six bytes in one transfer, temperature first: 0x1D..0x1F then
    // 0x20..0x22, each little-endian XLSB/LSB/MSB.
    uint8_t buf[6];
    if (!ReadRegs(REG_TEMP_XLSB, buf, sizeof(buf))) {
        return false;
    }

    const int32_t traw = (int32_t)(((uint32_t)buf[2] << 16) | ((uint32_t)buf[1] << 8) | buf[0]);
    const uint32_t praw = ((uint32_t)buf[5] << 16) | ((uint32_t)buf[4] << 8) | buf[3];

    // Sign-extend the temperature's 24 bits. Without this a sub-zero reading
    // becomes about +16777°C, which no plausibility check on altitude would
    // ever catch because pressure is unaffected.
    const int32_t tsigned = (traw & 0x800000) ? (traw - 0x1000000) : traw;

    // No calibration coefficients and no polynomial: the BMP580 compensates
    // on-chip and these two divisors are the whole conversion.
    const float c = (float)tsigned / 65536.0f;
    const float pa = (float)praw / 64.0f;

    if (!BaroAltitude_PressurePlausible(pa)) {
        return false;
    }

    if (pressure_pa != nullptr) {
        *pressure_pa = pa;
    }
    if (temperature_c != nullptr) {
        *temperature_c = c;
    }
    return true;
}

float Barometer_AltitudeM() { return s_altitude_m; }
bool Barometer_HaveAltitude() { return s_have_altitude; }

void Barometer_StartMonitor() {
    if (!s_found) {
        return;
    }
    xTaskCreatePinnedToCore(BaroTask, "baro", 3072, nullptr, 1, nullptr, 0);
}

bool Barometer_Reading(float *pressure_pa, float *temperature_c) {
    if (!s_have_altitude) {
        return false;
    }
    // Age matters more than the value. A cached reading with no expiry would
    // have quietly hidden the loose jumper this cache was written during: the
    // sensor stops answering, the settings page keeps showing the last good
    // pressure, and the panel reads healthy for the rest of the boot. Same
    // rule as every live reading on the dashboard (CLAUDE.md §8).
    if ((uint32_t)(millis() - s_last_ms) > STALE_MS) {
        return false;
    }
    if (pressure_pa != nullptr) {
        *pressure_pa = s_last_pa;
    }
    if (temperature_c != nullptr) {
        *temperature_c = s_last_c;
    }
    return true;
}
