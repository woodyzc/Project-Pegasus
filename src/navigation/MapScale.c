#include "MapScale.h"

#include <stddef.h>
#include <stdio.h>

/* The 1-2-5 sequence, smallest first. A metre is the floor: below that the
   bar is measuring less than the panel can distinguish anyway. */
static const uint32_t STEPS[] = {
    1,      2,      5,      10,      20,      50,      100,      200,     500,
    1000,   2000,   5000,   10000,   20000,   50000,   100000,   200000,  500000,
};
#define STEP_COUNT (sizeof(STEPS) / sizeof(STEPS[0]))

bool MapScale_Choose(double metres_per_pixel, int max_px, uint32_t *out_metres, int *out_px) {
    if (metres_per_pixel <= 0.0 || max_px <= 0) {
        return false;
    }

    const double span_m = metres_per_pixel * (double)max_px;

    /* Largest step that still fits. Walked upwards and remembered rather than
       searched downwards, so a span smaller than the first step still yields
       the first step rather than nothing -- a 1m bar drawn short is honest,
       where no bar at all looks like a fault. */
    uint32_t chosen = STEPS[0];
    for (size_t i = 0; i < STEP_COUNT; i++) {
        if ((double)STEPS[i] <= span_m) {
            chosen = STEPS[i];
        } else {
            break;
        }
    }

    int px = (int)(((double)chosen / metres_per_pixel) + 0.5);
    if (px < 1) {
        px = 1;
    }
    if (px > max_px) {
        px = max_px; /* only reachable for the floor step on an absurd zoom */
    }

    if (out_metres != NULL) {
        *out_metres = chosen;
    }
    if (out_px != NULL) {
        *out_px = px;
    }
    return true;
}

bool MapScale_Format(uint32_t metres, char *out, size_t out_size) {
    if (out == NULL || out_size < 8) {
        return false;
    }
    if (metres >= 1000u) {
        const uint32_t km = metres / 1000u;
        const uint32_t frac = (metres % 1000u) / 100u;
        if (frac == 0u) {
            snprintf(out, out_size, "%lu km", (unsigned long)km);
        } else {
            snprintf(out, out_size, "%lu.%lu km", (unsigned long)km, (unsigned long)frac);
        }
    } else {
        snprintf(out, out_size, "%lu m", (unsigned long)metres);
    }
    return true;
}
