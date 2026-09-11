#pragma once

#include <stdint.h>
#include <stddef.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

// Trip-distance accumulation, kept free of Arduino, LVGL and NVS so the rule
// that decides which GPS steps count can be tested on the host (see
// test/host/test_trip_accum.c).
//
// This logic used to live inside Page_Dashboard as three file-scope variables
// updated from a redraw callback. That put the odometer in the UI layer, where
// nothing could reach it: it could not be persisted without the UI knowing
// about NVS, and it could not be tested at all.
//
// It is NOT the Kalman filter CLAUDE.md §4 Task 1 asks for. The two thresholds
// below are a blunt stopgap that drops the obviously-wrong steps; a real filter
// would model velocity and reject drift by likelihood rather than by size.

#ifdef __cplusplus
extern "C" {
#endif

// A single fix cannot legitimately advance the trip by more than this. At 1Hz
// it would mean 3600 km/h, so a step this large is a cold fix settling, not a
// rider. Kept from the original dashboard code, where it was the only guard.
#define TRIP_MAX_STEP_M 1000.0

// ...and below this, a step is more likely receiver noise than movement.
//
// A stationary consumer GNSS wanders by a metre or two between samples, and
// without a floor those wanders all add: leave the bike outside a shop and the
// trip counter climbs on its own. The cost is real movement slower than about
// 3.6 km/h being dropped, which for a bike computer is a trade worth making --
// though it is exactly the kind of compromise the Kalman filter is meant to
// replace.
#define TRIP_MIN_STEP_M 1.0

typedef struct {
    double total_m;
    double prev_lat;
    double prev_lon;
    bool has_prev;
} TripAccum_t;

// Great-circle distance in metres.
double TripAccum_DistanceMetres(double lat1, double lon1, double lat2, double lon2);

// `initial_m` seeds the total, so a saved trip can resume across a reboot.
void TripAccum_Init(TripAccum_t *trip, double initial_m);

// Offers one fix. Returns the metres actually added, which is 0 when there is
// no fix, no previous fix to measure from, or the step fell outside the
// thresholds above.
//
// A fix that is rejected as noise still does NOT move the reference point:
// otherwise a series of sub-threshold steps in one direction would each be
// discarded while the rider slowly covered real ground. Only a step that
// counts advances the origin.
double TripAccum_AddFix(TripAccum_t *trip, bool fix_valid, double lat, double lon);

// Zeroes the distance and forgets the previous fix, so the next one starts a
// new measurement rather than counting the gap since the last.
void TripAccum_Reset(TripAccum_t *trip);

double TripAccum_Metres(const TripAccum_t *trip);

#ifdef __cplusplus
}
#endif
