#pragma once

#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

// Ride averages and maxima, kept free of Arduino, LVGL, NVS and DataCenter so
// the rules can be tested on the host (see test/host/test_ride_stats.c).
//
// The same split TripAccum has, for the same reason: the rule about which
// samples count is where the bugs are, and it is worth being able to run it on
// a laptop rather than inferring it from a photograph of a panel.

#ifdef __cplusplus
extern "C" {
#endif

// Below this a rider is stopped, not moving slowly.
//
// Average speed on a bike computer means moving average, and the distinction
// matters: a rider who stops for coffee should not watch their average decay.
// The threshold is the same order as TripAccum's distance floor, which drops
// steps below about 3.6 km/h, so the two agree about what counts as moving --
// otherwise stationary GNSS noise would add time to the denominator while
// adding no distance to the numerator, and a parked bike would show a falling
// average.
#define RIDE_STATS_MOVING_KMH 3.0f

// A speed no rider reaches, above which a sample is a bad fix rather than a
// sprint. A cold receiver settling can report hundreds of km/h for a sample or
// two, and a single one of those would sit in the "max" field for the rest of
// the ride, which is exactly the field a rider screenshots.
#define RIDE_STATS_MAX_PLAUSIBLE_KMH 120.0f

// Outside this a heart rate is a dropout or a decoding error. Straps report 0
// when they lose skin contact, and a strap doing that for one sample must not
// take the average down with it.
#define RIDE_STATS_MIN_BPM 25
#define RIDE_STATS_MAX_BPM 240

typedef struct {
    // Speed. The average is distance over moving time rather than a mean of
    // samples, because samples arrive at whatever rate the receiver feels like
    // and a mean of them weights a slow patch with dense fixes the same as a
    // fast one with sparse fixes.
    float max_kmh;
    double moving_seconds;
    double moving_metres;

    // Heart rate, averaged over samples, because a strap reports at a steady
    // rate and there is no distance to divide by.
    uint8_t max_bpm;
    uint32_t bpm_total;
    uint32_t bpm_samples;
} RideStats_t;

void RideStatsCore_Reset(RideStats_t *s);

// Feeds one speed sample and the seconds since the previous one. `dt_seconds`
// of zero or less is ignored, which is what a repeated timestamp looks like.
void RideStatsCore_AddSpeed(RideStats_t *s, float kmh, double dt_seconds);

void RideStatsCore_AddHeartRate(RideStats_t *s, uint8_t bpm);

// Moving average in km/h, or 0 when nothing has moved yet.
float RideStatsCore_AvgSpeedKmh(const RideStats_t *s);

// Mean heart rate, or 0 when no usable sample has arrived.
uint8_t RideStatsCore_AvgBpm(const RideStats_t *s);

#ifdef __cplusplus
}
#endif
