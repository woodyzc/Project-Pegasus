#pragma once

#include <stdbool.h>
#include <stdint.h>

// QMI8658 6-axis IMU, on the board's sensor I2C bus (hal/SensorBus.h).
//
// ---------------------------------------------------------------------------
// What it is here for
// ---------------------------------------------------------------------------
// One job: deciding whether the bike is moving, so the three consumers of a
// wandering GNSS position can ignore it. The reasoning, and what this
// deliberately does NOT attempt, is in system/Stillness.h -- in short, this
// cannot make a fix more accurate and does not try. It is a detector, not an
// integrator.
//
// The spec (CLAUDE.md §2) also lists inclination, anti-theft, fall detection
// and Any-Motion wake. Grade now comes from the barometer instead, which is
// the better instrument for it (system/Grade.h), and Any-Motion wake is not
// available at all: the part's interrupt lines are not brought out to a GPIO
// on this board -- Waveshare's own driver assigns no pin to them and polls.
//
// ---------------------------------------------------------------------------
// Register map: the vendor's, not inferred
// ---------------------------------------------------------------------------
// Taken from Waveshare's own Arduino demo (jeffvan302/WS_ESP32_Touch28,
// Gyro_QMI8658.cpp), the same source §2's pinout came from. That is the rule
// the touch bring-up paid for and the BMP580 driver followed: match the
// reference first, improve afterwards.
//
// Address 0x6B with SDO low, which is how this board wires it; 0x6A is the
// alternative and is probed as well. WHO_AM_I reads 0x05.
void Imu_Init();

// True once a QMI8658 answered and identified itself.
bool Imu_Found();

// The address that answered, or 0.
uint8_t Imu_Address();

// WHO_AM_I as read back. 0x05 is the part; anything else with a live address
// is a different device sharing it, which is worth seeing rather than
// silently treating as an IMU.
uint8_t Imu_WhoAmI();

// Spawns the Core 0 sampling task (CLAUDE.md §4). No-op when nothing was
// found. Preconditions: Imu_Init() has run.
void Imu_StartMonitor();

// ⚠️ **False when no IMU is fitted, and false once its readings go stale.**
// Both are the safe answer: every consumer gates on "is it still" rather than
// "is it moving", so a board without the part -- or with one that has stopped
// answering -- behaves exactly as it did before this existed.
//
// The staleness half is not decoration. Without it a part that fell off the
// bus while parked would leave "still" standing for the rest of the boot, and
// a whole ride would record no distance at all with nothing on the panel to
// say why.
bool Imu_IsStill();

// Whether the reading above is current at all. Shown on the settings page, so
// "fitted but silent" is distinguishable from "fitted and saying moving".
bool Imu_Fresh();

// Last sample, for the settings page: acceleration magnitude in g (about 1.0
// at rest in any orientation) and rotation magnitude in degrees per second.
// Both zero until the first read succeeds.
float Imu_AccelG();
float Imu_GyroDps();
