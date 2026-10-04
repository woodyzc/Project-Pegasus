#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Pack voltage to a percentage, and the filter in front of it.
//
// Pure arithmetic and no hardware: hal/Battery.cpp reads the ADC and feeds
// this, and test/host/test_battery_level.c is where it is checked.
//
// ---------------------------------------------------------------------------
// Why the reading needs filtering at all
// ---------------------------------------------------------------------------
// The curve below spends 40 percentage points between 3400mV and 3700mV, so
// in the range a rider actually cares about **one percent is 7.5 millivolts**.
// The pin sees a third of the pack through the divider, so 2.5mV of noise on
// the pin is a whole percent on the panel -- and ESP32-S3 ADC1 is noisier than
// that. Averaging a burst inside one reading is not enough: the burst is over
// in microseconds and sees one slice of the noise, not its mean.
//
// Hence a slow filter ACROSS readings. Battery voltage is the slowest signal
// on this device and nothing is lost by taking half a minute to believe a
// change.
//
// ---------------------------------------------------------------------------
// ⚠️ What the filter cannot fix, and must not be blamed for
// ---------------------------------------------------------------------------
// Plugging in USB moves the reading sharply upward -- 10% to 38% has been seen
// in about a second. That is **real terminal voltage**, not noise: the charger
// starts pushing current and the pack's internal resistance turns that
// current into volts at the terminals. The pack did not gain charge; it
// gained a charger.
//
// Nothing here can tell those apart, because the only thing this firmware can
// see is the voltage. The board has no readable charge-status line -- the red
// LED is wired to the charger IC, not to a GPIO -- and `Battery_t.on_usb`,
// which looks like it would answer this, is a 4500mV threshold on a pack that
// terminates at 4.2V, so it can never be true (CLAUDE.md §8). The filter slows
// the jump down; it cannot make it mean something different.

// Percentage from pack millivolts, 0 for "no reading yet".
//
// Piecewise linear rather than a single 3.0-4.2V ramp, because a LiPo spends
// most of its charge above 3.7V and a straight map badly overstates what is
// left once it drops below nominal. Points from Waveshare's power_manager.
uint8_t BatteryLevel_PercentFromMillivolts(uint16_t millivolts);

// Weight given to each new reading. At the 5s publish period this is a time
// constant around half a minute: slow enough to sit still, far faster than a
// battery actually discharges.
#define BATTERY_SMOOTH_ALPHA 0.15f

typedef struct {
    float millivolts;
    bool have;
} BatterySmoother_t;

void BatterySmoother_Reset(BatterySmoother_t *s);

// Folds one reading in and returns the smoothed millivolts.
//
// The first reading is taken whole. Filtering up from zero would walk the
// panel from 0% to the real level over the first minute of every boot, which
// reads as a battery charging at impossible speed.
uint16_t BatterySmoother_Push(BatterySmoother_t *s, uint16_t millivolts);

#ifdef __cplusplus
}
#endif
