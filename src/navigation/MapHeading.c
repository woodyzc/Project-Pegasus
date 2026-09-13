#include "MapHeading.h"

#include <math.h>
#include <stddef.h>

static float Normalise(float deg) {
    // fmodf can return a negative for a negative input, so the second step is
    // not redundant.
    deg = fmodf(deg, 360.0f);
    if (deg < 0.0f) {
        deg += 360.0f;
    }
    return deg;
}

float MapHeading_Delta(float a, float b) {
    float d = Normalise(a - b);
    if (d > 180.0f) {
        d -= 360.0f;
    }
    return d;
}

void MapHeading_Reset(MapHeading_t *h) {
    if (h == NULL) {
        return;
    }
    h->valid = false;
    h->heading_deg = 0.0f;
    h->drawn_deg = 0.0f;
    h->has_drawn = false;
}

bool MapHeading_Feed(MapHeading_t *h, bool fix_valid, float speed_mps, float heading_deg) {
    if (h == NULL) {
        return false;
    }
    if (!fix_valid) {
        return false;
    }
    // Reject a heading that is not a heading. A receiver with no course
    // solution can report anything, including NaN, and the comparison below is
    // written to reject on false so NaN falls out here.
    if (!(heading_deg >= 0.0f && heading_deg <= 360.0f)) {
        return false;
    }
    // Held, not updated. The last heading from when the rider was actually
    // moving is a far better guess at which way they are facing than the
    // direction of the receiver's noise while they stand at a light.
    if (!(speed_mps >= MAP_HEADING_MIN_MPS)) {
        return false;
    }

    const float sample = Normalise(heading_deg);

    if (!h->valid) {
        h->valid = true;
        h->heading_deg = sample;
    } else {
        // Smoothed along the SHORT way round. Interpolating the raw numbers
        // would take 350 towards 10 through 180 -- the map spinning a full
        // half-turn to follow a rider who drifted a few degrees across north.
        h->heading_deg = Normalise(h->heading_deg +
                                   MAP_HEADING_ALPHA * MapHeading_Delta(sample, h->heading_deg));
    }

    if (!h->has_drawn) {
        h->has_drawn = true;
        h->drawn_deg = h->heading_deg;
        return true;
    }

    if (fabsf(MapHeading_Delta(h->heading_deg, h->drawn_deg)) >= MAP_HEADING_REDRAW_DEG) {
        h->drawn_deg = h->heading_deg;
        return true;
    }
    return false;
}

bool MapHeading_Valid(const MapHeading_t *h) {
    return (h != NULL) && h->valid;
}

float MapHeading_Degrees(const MapHeading_t *h) {
    return (h != NULL && h->valid) ? h->heading_deg : 0.0f;
}
