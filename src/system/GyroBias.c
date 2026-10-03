#include "GyroBias.h"

#include <math.h>
#include <stddef.h>

static float Magnitude(const float v[3]) {
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

void GyroBias_Reset(GyroBias_t *b) {
    if (b == NULL) {
        return;
    }
    b->bias[0] = 0.0f;
    b->bias[1] = 0.0f;
    b->bias[2] = 0.0f;
    b->have = false;
}

float GyroBias_Correct(GyroBias_t *b, const float gyro_dps[3], bool accel_quiet) {
    if (b == NULL || gyro_dps == NULL) {
        return 0.0f;
    }

    float corrected[3];
    for (int i = 0; i < 3; i++) {
        corrected[i] = gyro_dps[i] - b->bias[i];
    }
    const float corrected_mag = Magnitude(corrected);

    if (accel_quiet) {
        // Refused while the part appears to be rotating. Written to reject on
        // false, so a NaN anywhere lands here and leaves the estimate alone.
        const bool plausible = (corrected_mag <= GYRO_BIAS_ADAPT_MAX_DPS);
        if (plausible) {
            if (!b->have) {
                // Seeded whole from the first quiet sample rather than filtered
                // up from zero, which would spend the first minute subtracting
                // a fraction of the offset and reporting the remainder as
                // rotation.
                for (int i = 0; i < 3; i++) {
                    b->bias[i] = gyro_dps[i];
                }
                b->have = true;
            } else {
                for (int i = 0; i < 3; i++) {
                    b->bias[i] += GYRO_BIAS_ALPHA * (gyro_dps[i] - b->bias[i]);
                }
            }
        }
    }

    // An estimate this large is a fault, not an offset, and subtracting it
    // would hide real rotation. Hand back the raw magnitude instead.
    if (Magnitude(b->bias) > GYRO_BIAS_MAX_DPS) {
        return Magnitude(gyro_dps);
    }

    // Recomputed, because the estimate may have moved above.
    for (int i = 0; i < 3; i++) {
        corrected[i] = gyro_dps[i] - b->bias[i];
    }
    return Magnitude(corrected);
}

float GyroBias_MagnitudeDps(const GyroBias_t *b) {
    return (b != NULL) ? Magnitude(b->bias) : 0.0f;
}
