#pragma once

#include <stdbool.h>
#include <stdint.h>

// BMP580 barometric pressure sensor, on the board's sensor I2C bus.
//
// ---------------------------------------------------------------------------
// Wiring and address
// ---------------------------------------------------------------------------
// SDA 11 / SCL 10 -- the bus that already carries the PCF85063 RTC at 0x51 and
// the QMI8658 IMU at 0x6B, brought out on both the 4-pin I2C connector and the
// 12-pin one. The module is a breakout with pull resistors on CS and SDO, so
// it comes up in I2C mode at **0x47** with only VCC, GND, SDA and SCL
// connected; 0x46 is the same part with SDO pulled low. Both are probed,
// because that choice belongs to the breakout rather than to this firmware --
// verified on the bench 2026-09-26, where the bus scan read "47 51 6B".
//
// ---------------------------------------------------------------------------
// Why this part and not the GNSS
// ---------------------------------------------------------------------------
// A receiver's altitude is its weakest axis -- every satellite is above it, so
// the vertical geometry is poor and ±10m of wander while stationary is
// ordinary. `Ascent.h` already carries a threshold to stop that wander being
// counted as climbing, and that threshold is precisely the resolution being
// given up. A barometer measures *change* to centimetres, which is what a
// climb total is actually made of.
//
// ---------------------------------------------------------------------------
// Register map: Bosch, not inferred
// ---------------------------------------------------------------------------
// Taken from boschsensortec/BMP5_SensorAPI (bmp5_defs.h, bmp5.c) rather than
// worked out from behaviour. That is deliberate: on 2026-09-26 three separate
// symptoms in the touch bring-up turned out to be deviations from the vendor's
// driver rather than faults in the part, and each was investigated as though
// it were a mystery. CLAUDE.md §2 carries the rule -- match the reference
// first, improve afterwards.
//
// The one thing worth knowing up front: **the BMP580 compensates internally.**
// Unlike the BMP280 lineage there are no calibration coefficients to read and
// no polynomial to apply. Temperature is a signed 24-bit count over 65536 in
// degrees C; pressure is an unsigned 24-bit count over 64 in pascals.

// Probes 0x47 then 0x46, checks the chip ID, and configures a continuous
// measurement. Safe to call when nothing is fitted -- it simply reports not
// found and every accessor below stays quiet.
//
// Begins the sensor bus itself. Nothing else in this firmware drives it.
void Barometer_Init();

// True once a BMP580 answered and returned a chip ID this driver recognises.
bool Barometer_Found();

// The 7-bit address that answered: 0x47, 0x46, or 0 when nothing did.
uint8_t Barometer_Address();

// The chip ID read back. 0x50 is the BMP580's primary ID and 0x51 its
// secondary; anything else with a live address means a part sharing the
// address but not the register map, which is worth seeing rather than
// silently treating as a barometer.
uint8_t Barometer_ChipId();

// Reads one pressure/temperature sample **from the bus**.
//
// Takes the driver's lock, so it is safe to call from any task -- but it
// blocks for the length of an I2C transfer, so it does not belong on the
// render path. The UI wants Barometer_Reading() below.
//
// Returns false when no sensor is fitted, the transfer fails, or the pressure
// is outside the part's own 30..125kPa range -- the last of which is a failed
// read rather than weather, and must not reach an accumulator.
bool Barometer_Read(float *pressure_pa, float *temperature_c);

// The last *recent* good sample, cached by the sampling task. No bus traffic
// and no blocking, which is what the UI should use: a screen wanting a number
// should read the number the sampler already has rather than opening its own
// transaction on a bus another task is driving.
//
// Returns false until the first successful read, **and again once the newest
// sample goes stale**. That second half is the important one. A loose jumper
// on the bench put "found but not reading" on the settings page and that is
// how the wiring fault was found; a cache without an expiry would have shown
// the last good pressure indefinitely and reported a dead sensor as healthy.
// Ages out for the same reason the dashboard's readings do (CLAUDE.md §8).
bool Barometer_Reading(float *pressure_pa, float *temperature_c);

// Smoothed altitude in metres above the standard-atmosphere sea level, from
// the most recent successful read. Zero when nothing has been read yet.
//
// Absolute height here drifts with the weather -- tens of metres across a day
// as a front passes -- so this is a figure to take differences of, not one to
// compare against a map. `Barometer_HaveAltitude()` says whether it means
// anything yet.
float Barometer_AltitudeM();
bool Barometer_HaveAltitude();

// Spawns the Core 0 sampling task (CLAUDE.md §4). Preconditions:
// Barometer_Init() has run. A no-op when no sensor was found.
void Barometer_StartMonitor();
