#include "BaroAltitude.h"

#include <math.h>
#include <stddef.h>

// The BMP580's specified measurement range, from Bosch's datasheet. Used to
// reject a failed read rather than to judge the weather.
#define BARO_MIN_PA 30000.0f
#define BARO_MAX_PA 125000.0f

bool BaroAltitude_PressurePlausible(float pressure_pa) {
    // NaN fails both comparisons, which is the point: an unchecked NaN here
    // would reach the climb accumulator and make every later total NaN too,
    // with nothing on the panel to say when it started.
    return (pressure_pa >= BARO_MIN_PA) && (pressure_pa <= BARO_MAX_PA);
}

float BaroAltitude_MetresFromPa(float pressure_pa) {
    if (!BaroAltitude_PressurePlausible(pressure_pa)) {
        return 0.0f;
    }

    // h = 44330 * (1 - (P/P0)^(1/5.255))
    //
    // The exponent is 1/5.255, not 5.255. Inverting it by accident still
    // produces a smooth, plausible-looking curve -- it just reads about
    // 44330m at sea level, which is why this is worth a test rather than an
    // eyeball.
    const float ratio = pressure_pa / BARO_SEA_LEVEL_PA;
    return 44330.0f * (1.0f - powf(ratio, 1.0f / 5.255f));
}

void BaroSmoother_Reset(BaroSmoother_t *s) {
    if (s == NULL) {
        return;
    }
    s->altitude_m = 0.0f;
    s->have = false;
}

float BaroSmoother_Push(BaroSmoother_t *s, float altitude_m, float alpha) {
    if (s == NULL) {
        return altitude_m;
    }

    if (!s->have) {
        // Seeded with the first real sample rather than filtered up from
        // zero. Filtering from zero would walk the output from sea level to
        // the rider's actual altitude over the first minute, and a climb
        // total integrating that would credit them the whole way.
        s->altitude_m = altitude_m;
        s->have = true;
        return s->altitude_m;
    }

    if (alpha <= 0.0f) {
        return s->altitude_m; // frozen; a caller asking for no new information
    }
    if (alpha >= 1.0f) {
        s->altitude_m = altitude_m; // no smoothing at all
        return s->altitude_m;
    }

    s->altitude_m += alpha * (altitude_m - s->altitude_m);
    return s->altitude_m;
}
