#include "TripAccum.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

double TripAccum_DistanceMetres(double lat1, double lon1, double lat2, double lon2) {
    const double kEarthRadiusM = 6371000.0;
    const double kDegToRad = M_PI / 180.0;

    const double dlat = (lat2 - lat1) * kDegToRad;
    const double dlon = (lon2 - lon1) * kDegToRad;
    const double a = sin(dlat / 2.0) * sin(dlat / 2.0) +
                     cos(lat1 * kDegToRad) * cos(lat2 * kDegToRad) * sin(dlon / 2.0) *
                         sin(dlon / 2.0);
    return 2.0 * kEarthRadiusM * atan2(sqrt(a), sqrt(1.0 - a));
}

void TripAccum_Init(TripAccum_t *trip, double initial_m) {
    if (trip == NULL) {
        return;
    }
    // A stored total that is negative or not a number would poison every
    // reading that followed, and NVS can return anything if it was written by
    // an older build. Treat only a sane value as a resumable trip.
    trip->total_m = (initial_m > 0.0 && initial_m < 1.0e9) ? initial_m : 0.0;
    trip->prev_lat = 0.0;
    trip->prev_lon = 0.0;
    // Deliberately no previous fix, even when resuming: the rider may be
    // hundreds of kilometres from where the trip was last saved, and counting
    // that gap as distance ridden is exactly the bug persistence would
    // otherwise introduce.
    trip->has_prev = false;
}

double TripAccum_AddFix(TripAccum_t *trip, bool fix_valid, double lat, double lon) {
    if (trip == NULL || !fix_valid) {
        return 0.0;
    }

    if (!trip->has_prev) {
        trip->prev_lat = lat;
        trip->prev_lon = lon;
        trip->has_prev = true;
        return 0.0;
    }

    const double step_m = TripAccum_DistanceMetres(trip->prev_lat, trip->prev_lon, lat, lon);

    if (step_m >= TRIP_MAX_STEP_M) {
        // A jump this large is the receiver, not the rider. Take the new
        // position as the truth -- it is where we are now -- but do not credit
        // the leap as distance travelled.
        trip->prev_lat = lat;
        trip->prev_lon = lon;
        return 0.0;
    }

    if (step_m < TRIP_MIN_STEP_M) {
        // Noise. Leave the reference point where it is so that slow, genuine
        // movement still accumulates once it has covered enough ground.
        return 0.0;
    }

    trip->total_m += step_m;
    trip->prev_lat = lat;
    trip->prev_lon = lon;
    return step_m;
}

void TripAccum_Reset(TripAccum_t *trip) {
    if (trip == NULL) {
        return;
    }
    trip->total_m = 0.0;
    trip->has_prev = false;
}

double TripAccum_Metres(const TripAccum_t *trip) {
    return trip == NULL ? 0.0 : trip->total_m;
}
