#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Pressure to altitude, and the smoothing that makes a barometer usable for
// a climb total.
//
// Pure arithmetic, no hardware: the BMP580 driver in hal/Barometer.h feeds
// this, and test/host/test_baro_altitude.c is where it is actually checked.
//
// ---------------------------------------------------------------------------
// Why a barometer at all, when there is already a GNSS
// ---------------------------------------------------------------------------
// A GNSS fix knows its altitude far worse than its position -- the satellites
// are all above the receiver, so the vertical geometry is the weak axis, and
// ±10m of wander on a stationary receiver is ordinary. Ascent.h already
// carries a threshold to keep that wander from being counted as climbing, and
// that threshold is exactly the accuracy being thrown away: a real 4m rise is
// indistinguishable from noise. A barometer resolves centimetres of *change*,
// which is the quantity a climb total is made of.
//
// The trade runs the other way for absolute height: this reports altitude
// against a fixed sea-level reference, so a passing weather system moves the
// reading by tens of metres over a day. That is why the API below separates
// the two ideas.

// Standard atmosphere, ISA. The reference pressure a "0m" reading means.
#define BARO_SEA_LEVEL_PA 101325.0f

// Metres above the standard-atmosphere sea level, from pressure in pascals.
//
// The international barometric formula, the same one aviation altimeters use
// below the tropopause:
//
//     h = 44330 * (1 - (P/P0)^(1/5.255))
//
// Good to a few metres through the troposphere, which is all a bicycle needs.
// Returns 0 for a non-positive or absurd pressure rather than a NaN that
// would poison every accumulator downstream.
float BaroAltitude_MetresFromPa(float pressure_pa);

// Is this pressure reading believable at all?
//
// The BMP580's own range is 30..125kPa. Anything outside that is a failed
// read or a decode fault, not weather -- 30kPa is roughly the summit of
// Everest and 125kPa is below the Dead Sea.
bool BaroAltitude_PressurePlausible(float pressure_pa);

// ---------------------------------------------------------------------------
// The smoother
// ---------------------------------------------------------------------------
// A barometer's noise is small but not zero, and a climb total integrates
// every wiggle: an unsmoothed sensor sitting still on a desk will happily
// accumulate tens of metres an hour. The filter here is a first-order low
// pass, which costs one multiply and no history buffer.
//
// It is NOT the same job as Ascent.h's threshold. That one rejects large GNSS
// excursions; this one removes small sensor noise before anything integrates
// it. Both can be wanted at once.
typedef struct {
    float altitude_m;
    bool have;
} BaroSmoother_t;

void BaroSmoother_Reset(BaroSmoother_t *s);

// Folds one altitude sample in and returns the smoothed value.
//
// `alpha` is the weight given to the new sample, 0..1: smaller is smoother
// and laggier. The first sample is taken whole -- starting from zero would
// otherwise make the filter climb from sea level to wherever the rider is,
// and a climb total would count all of it.
float BaroSmoother_Push(BaroSmoother_t *s, float altitude_m, float alpha);

#ifdef __cplusplus
}
#endif
