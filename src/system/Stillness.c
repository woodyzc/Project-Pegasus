#include "Stillness.h"

#include <math.h>
#include <stddef.h>

void Stillness_Reset(Stillness_t *s) {
    if (s == NULL) {
        return;
    }
    s->still = false;
    s->have = false;
    s->quiet_since_ms = 0;
}

bool Stillness_Feed(Stillness_t *s, float accel_g_mag, float gyro_dps_mag, uint32_t t_ms) {
    if (s == NULL) {
        return false;
    }

    // Written to reject on false, so a NaN on either input lands here and
    // reads as movement. A sensor that has started producing nonsense must not
    // be able to freeze the odometer.
    const float from_gravity = fabsf(accel_g_mag - 1.0f);
    const bool quiet =
        (from_gravity <= STILL_ACCEL_TOL_G) && (gyro_dps_mag <= STILL_GYRO_TOL_DPS);

    if (!quiet) {
        // Believed immediately -- see the note on STILL_HOLD_MS.
        s->still = false;
        s->have = true;
        s->quiet_since_ms = t_ms;
        return false;
    }

    if (!s->have) {
        s->have = true;
        s->quiet_since_ms = t_ms;
        return false;
    }

    // Unsigned, so the 49-day tick wrap yields the real elapsed time rather
    // than an enormous one that would declare stillness instantly.
    if (!s->still && (uint32_t)(t_ms - s->quiet_since_ms) >= STILL_HOLD_MS) {
        s->still = true;
    }
    return s->still;
}

bool Stillness_IsStill(const Stillness_t *s) { return (s != NULL) && s->still; }
