#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Is the bike moving at all? Decided from the IMU, not from the receiver.
//
// Pure arithmetic and no hardware: hal/Imu.h feeds this and
// test/host/test_stillness.c is where it is actually checked.
//
// ---------------------------------------------------------------------------
// What this is for, and what it is not
// ---------------------------------------------------------------------------
// A stationary GNSS receiver does not report a stationary position. It wanders
// -- metres per sample is ordinary -- and that wander reaches three places
// that each treat it as real movement:
//
//   * SPEED shows a figure on a parked bike, because nothing clamps it;
//   * the odometer creeps, because TripAccum only rejects steps under
//     TRIP_MIN_STEP_M (1m) and the wander routinely exceeds that;
//   * the idle timer never expires, because PowerManager counts anything over
//     MOVING_MPS as activity, so the screen never blanks and deep sleep is
//     never reached.
//
// ⚠️ **The IMU cannot fix the fix.** It has no idea where it is, and
// integrating acceleration for position drifts uselessly within seconds.
// Nothing here makes a wandering position more accurate. What it does is say,
// with near-certainty, that the wander is not movement -- so the three
// consumers above can ignore it.
//
// That is the one job a 6-axis IMU is genuinely excellent at: it is a
// detector rather than an integrator, so it has no drift of its own.
//
// ---------------------------------------------------------------------------
// Why not simply clamp slow speeds to zero
// ---------------------------------------------------------------------------
// Because a fixed threshold has to choose. Set it high enough to swallow
// drift and it also swallows a rider walking the bike up a path at 0.8 m/s;
// set it low and the drift gets through. The IMU breaks that tie -- it can
// tell a parked bike with a noisy fix from one genuinely moving slowly, which
// no amount of filtering on the speed alone can do.
//
// ---------------------------------------------------------------------------
// Orientation-free on purpose
// ---------------------------------------------------------------------------
// The test is on the MAGNITUDE of acceleration against 1g, not on any axis. A
// head unit can be clamped to the bars at any angle, and gravity is 1g in
// whatever direction that happens to be -- so this needs no mounting
// calibration and no zeroing, and cannot be defeated by remounting it.

// How far the acceleration magnitude may stray from 1g and still count as at
// rest. Generous enough for a bike rocking slightly on its stand or a rider
// straddling it at a light; far short of a wheel turning.
#define STILL_ACCEL_TOL_G 0.12f

// And how much rotation. A bike being pedalled, pushed or leaned produces far
// more than this on at least one axis.
#define STILL_GYRO_TOL_DPS 8.0f

// ⚠️ Deliberately asymmetric. Stillness has to be sustained before it is
// believed, because one quiet sample happens constantly mid-ride -- at the top
// of a pedal stroke, or coasting on smooth tarmac. Movement is believed at
// once, because the cost of the two mistakes is not equal: a late "moving"
// throws away real metres from the odometer, where a late "still" costs
// nothing but a second of drift nobody was watching.
#define STILL_HOLD_MS 2000u

typedef struct {
    bool still;
    bool have;
    uint32_t quiet_since_ms;
} Stillness_t;

void Stillness_Reset(Stillness_t *s);

// Folds in one sample and returns whether the bike is now considered at rest.
//
// `accel_g_mag` is the magnitude of the acceleration vector in g (about 1.0
// at rest, in any orientation) and `gyro_dps_mag` the magnitude of the
// rotation rate in degrees per second. `t_ms` is a free-running tick and may
// wrap.
//
// A NaN on either input reads as movement, which is the safe answer: it
// gates nothing, so a broken sensor degrades to today's behaviour rather than
// freezing the odometer.
bool Stillness_Feed(Stillness_t *s, float accel_g_mag, float gyro_dps_mag, uint32_t t_ms);

bool Stillness_IsStill(const Stillness_t *s);

#ifdef __cplusplus
}
#endif
