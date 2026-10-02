#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The board's sensor I2C bus: SDA 11 / SCL 10.
//
// Separate from the touch bus (hal/Touch.h), and shared by everything hanging
// off the 4-pin and 12-pin connectors: the BMP580 barometer at 0x47, the
// QMI8658 IMU at 0x6B, and the PCF85063 RTC at 0x51.
//
// ---------------------------------------------------------------------------
// Why this exists rather than each driver calling Wire itself
// ---------------------------------------------------------------------------
// Arduino's Wire is not thread-safe, and this firmware drives these parts from
// different cores: the barometer samples on Core 0, the IMU on Core 0, and the
// settings page reads from the LVGL task on Core 1. Two transfers interleaved
// on one bus corrupt each other -- a repeated START from one lands inside the
// other's transaction and both come back wrong, with no error anywhere.
//
// The lock used to live inside Barometer.cpp, which was safe only for exactly
// as long as the barometer was the only device driven. It guarded nothing the
// moment a second driver appeared, so it moved here before the IMU landed
// rather than after.
//
// Every call below takes the lock for the whole transfer. Callers never see it,
// which is the point: there is no ordering for a caller to get wrong.

// Idempotent. Safe to call from each driver's init, in any order.
void SensorBus_Begin();

// True if a device acknowledges its address.
bool SensorBus_Probe(uint8_t addr);

// Register read with a repeated START, which is what these parts expect: a
// STOP between the address write and the read detaches the two and the device
// clocks out whatever it feels like.
bool SensorBus_Read(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len);

bool SensorBus_Write(uint8_t addr, uint8_t reg, uint8_t value);
