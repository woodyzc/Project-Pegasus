#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Zero-rate offset removal for the gyro.
//
// Pure arithmetic and no hardware: hal/Imu.cpp feeds this and
// test/host/test_gyro_bias.c is where it is actually checked.
//
// ---------------------------------------------------------------------------
// The problem
// ---------------------------------------------------------------------------
// A MEMS gyro reports rotation when it is perfectly still. The bench unit
// reads **3.2 dps at rest**, against the 8 dps that system/Stillness.h treats
// as the boundary between parked and moving -- so 40% of that budget is spent
// before the bike has done anything. That is not noise, which averages out;
// it is a systematic offset that varies between parts and with temperature.
//
// Nothing has gone wrong yet because this particular part sits at 3.2. A
// warmer one, or a different one, sits higher, and the failure when it crosses
// 8 is silent and total: the board reads "moving" for ever while parked, the
// drift gating never engages, and the odometer creeps exactly as it did before
// any of this existed.
//
// ---------------------------------------------------------------------------
// Why not the vendor's approach
// ---------------------------------------------------------------------------
// Waveshare's driver averages 256 samples at startup and subtracts that,
// printing "keep device still". That is right for a demo and wrong here: this
// board is switched on by a rider, sometimes mid-ride, sometimes while being
// carried. Calibrating against motion bakes that motion in permanently, and
// the failure is invisible -- the number just stops meaning what it says.
//
// So the estimate is adaptive instead, and only moves when the ACCELEROMETER
// agrees nothing is happening. Gravity is the reference that cannot be
// argued with: a part being accelerated or rotated does not read 1g.
//
// ---------------------------------------------------------------------------
// The guard that matters
// ---------------------------------------------------------------------------
// ⚠️ A bias estimator that learns real rotation as "bias" is worse than none:
// it would subtract genuine movement and report the bike as still, which
// stops the odometer. So adaptation is refused whenever the apparent rotation
// is larger than a resting part could plausibly produce, and the estimate
// itself is refused outright beyond GYRO_BIAS_MAX_DPS -- past that something
// is wrong and an uncorrected reading is the safer answer.

// Adaptation weight per sample. At the IMU's 20Hz this is a time constant of
// a few seconds, which tracks thermal drift and is far too slow to chase a
// rider.
#define GYRO_BIAS_ALPHA 0.01f

// Apparent rotation above this is taken as real movement, and the estimate is
// left alone. Comfortably above any resting offset, far below pedalling.
#define GYRO_BIAS_ADAPT_MAX_DPS 10.0f

// An estimate beyond this is not believed at all. A part reading 25 dps at
// rest is faulty, and subtracting that much would hide real rotation.
#define GYRO_BIAS_MAX_DPS 25.0f

// A known, narrow hole, recorded rather than fixed: a rotation held steadily
// between GYRO_BIAS_ADAPT_MAX_DPS and the point where the accelerometer stops
// reading 1g would be learned as bias. It needs a sustained smooth turn at
// 8-10 dps with no lateral acceleration, which a bicycle does not really
// produce, and it self-corrects as soon as the rotation stops. Closing it
// would mean a second threshold to get wrong; it is cheaper to know about.

typedef struct {
    float bias[3];
    bool have;
} GyroBias_t;

void GyroBias_Reset(GyroBias_t *b);

// Folds in one sample and returns the bias-corrected rotation magnitude in
// degrees per second.
//
// `accel_quiet` should be true when the acceleration magnitude is close to 1g
// -- that is the caller's evidence that nothing is happening to the part, and
// the only condition under which the estimate is allowed to move.
// A null argument returns a large magnitude rather than zero: zero would read
// as "not rotating", and the safe answer to a question this cannot answer is
// movement.
float GyroBias_Correct(GyroBias_t *b, const float gyro_dps[3], bool accel_quiet);

// Magnitude of the current estimate, for the settings page. Zero until the
// first quiet sample.
float GyroBias_MagnitudeDps(const GyroBias_t *b);

#ifdef __cplusplus
}
#endif
