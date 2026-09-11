#include "HrZone.h"

#include <math.h>

const double HR_ZONE_UPPER_FRACTION[HR_ZONE_COUNT] = {0.30, 0.40, 0.60, 0.90, 1.00};

// True when the pair is usable. NVS, a settings page or a future auto-detected
// resting rate can all hand over nonsense, and every function below would
// otherwise divide by zero or run off the end of a zone table.
static bool Usable(uint8_t rest_bpm, uint8_t max_bpm) {
    return max_bpm > rest_bpm;
}

static double Reserve(uint8_t rest_bpm, uint8_t max_bpm) {
    return (double)max_bpm - (double)rest_bpm;
}

// Upper edge of `zone` as a real number of beats, before rounding.
static double UpperEdge(int zone, uint8_t rest_bpm, uint8_t max_bpm) {
    return (double)rest_bpm + HR_ZONE_UPPER_FRACTION[zone] * Reserve(rest_bpm, max_bpm);
}

int HrZone_Index(uint8_t bpm, uint8_t rest_bpm, uint8_t max_bpm) {
    if (!Usable(rest_bpm, max_bpm)) {
        return 0;
    }

    for (int zone = 0; zone < HR_ZONE_COUNT; zone++) {
        // floor(), matching HrZone_UpperBpm: the phone shows zone 1 topping
        // out at 87 for an edge computed as 87.9, so a reading of 87 is zone 1
        // and 88 is zone 2.
        if ((double)bpm <= floor(UpperEdge(zone, rest_bpm, max_bpm))) {
            return zone;
        }
    }
    return HR_ZONE_COUNT - 1;
}

double HrZone_Fraction(uint8_t bpm, uint8_t rest_bpm, uint8_t max_bpm) {
    if (!Usable(rest_bpm, max_bpm)) {
        return 0.0;
    }

    const double position = ((double)bpm - (double)rest_bpm) / Reserve(rest_bpm, max_bpm);
    if (position < 0.0) {
        return 0.0;
    }
    if (position > 1.0) {
        return 1.0;
    }
    return position;
}

double HrZone_SpanFraction(int zone) {
    if (zone < 0 || zone >= HR_ZONE_COUNT) {
        return 0.0;
    }
    const double lower = (zone == 0) ? 0.0 : HR_ZONE_UPPER_FRACTION[zone - 1];
    return HR_ZONE_UPPER_FRACTION[zone] - lower;
}

uint8_t HrZone_LowerBpm(int zone, uint8_t rest_bpm, uint8_t max_bpm) {
    if (zone <= 0 || zone >= HR_ZONE_COUNT || !Usable(rest_bpm, max_bpm)) {
        return rest_bpm;
    }
    // One beat above the zone below, so the bands are contiguous and do not
    // overlap -- which is how the phone presents them.
    return (uint8_t)(floor(UpperEdge(zone - 1, rest_bpm, max_bpm)) + 1.0);
}

uint8_t HrZone_UpperBpm(int zone, uint8_t rest_bpm, uint8_t max_bpm) {
    if (zone < 0 || zone >= HR_ZONE_COUNT || !Usable(rest_bpm, max_bpm)) {
        return max_bpm;
    }
    if (zone == HR_ZONE_COUNT - 1) {
        return max_bpm;
    }
    return (uint8_t)floor(UpperEdge(zone, rest_bpm, max_bpm));
}
