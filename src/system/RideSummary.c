#include "RideSummary.h"

#include <stdio.h>

// Past this the display is meaningless anyway, and it is far beyond any ride:
// 99 hours is four days in the saddle. Clamping keeps the output inside
// RIDE_SUMMARY_TIME_MAX rather than letting a stuck timer overflow it.
#define MAX_SECONDS (99u * 3600u + 59u * 60u + 59u)

bool RideSummary_FormatDuration(uint32_t seconds, char *out, size_t out_size) {
    if (out == NULL || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    if (out_size < RIDE_SUMMARY_TIME_MAX) {
        return false;
    }

    if (seconds > MAX_SECONDS) {
        seconds = MAX_SECONDS;
    }

    const unsigned hours = (unsigned)(seconds / 3600u);
    const unsigned minutes = (unsigned)((seconds / 60u) % 60u);
    const unsigned secs = (unsigned)(seconds % 60u);

    int written;
    if (hours > 0) {
        written = snprintf(out, out_size, "%u:%02u:%02u", hours, minutes, secs);
    } else {
        // Minutes unpadded: "7:05", not "07:05". The second field is padded
        // because it is a remainder of the first, and the first is not because
        // nothing precedes it.
        written = snprintf(out, out_size, "%u:%02u", minutes, secs);
    }

    if (written < 0 || (size_t)written >= out_size) {
        out[0] = '\0';
        return false;
    }
    return true;
}

bool RideSummary_IsEmpty(const RideSummary_t *s) {
    if (s == NULL) {
        return true;
    }
    // Distance alone is not enough: a ride can be all distance and no recorded
    // moving time if every fix arrived in one burst, and one second of moving
    // time with no distance is a rider who never left the driveway. Either one
    // being present makes the report worth showing.
    return !(s->distance_km > 0.0 || s->moving_seconds >= 1.0);
}
