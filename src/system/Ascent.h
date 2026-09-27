#pragma once

#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

// Total climbing over a ride, from GNSS altitude.
//
// Pure, like TripAccum and RideStatsCore and for the same reason: the whole
// feature is a rule about which changes in height are real, and that rule can
// only be checked by feeding it sequences a bench cannot produce.
//
// ---------------------------------------------------------------------------
// Why this is not a sum of the positive differences
// ---------------------------------------------------------------------------
// GNSS altitude is the worst number a receiver produces. The satellites are
// all above the horizon, so the vertical geometry is far weaker than the
// horizontal, and a few metres of wander between consecutive fixes is normal
// on a perfectly good fix. Adding up every rise would turn an hour of flat
// riding into several hundred metres of climbing -- not a small error, a
// completely fictional number, and one that looks plausible enough to believe.
//
// So there are three filters, and each removes a different lie:
//
//   1. Implausible altitudes are dropped outright. A receiver settling can
//      report kilometres underground.
//   2. Implausible vertical *rates* are dropped. A single fix that jumps 40m
//      is a spike, and a cyclist does not climb at 6 m/s. This needs the time
//      between samples, which is why Feed takes it.
//   3. What survives is smoothed, and then only counted through a hysteresis
//      band: the reference height moves only when the smoothed altitude
//      leaves the band, so wander inside it accumulates nothing at all.
//
// The band is what makes the number honest, and it costs something: a real
// change of direction loses up to a band's worth of climb. That undercount is
// the right trade against an overcount that grows without limit.
// ---------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

// Altitudes outside this are not a place a bicycle is. Below sea level is
// legitimate (the Dead Sea road is -400m), so the floor is generous.
#define ASCENT_MIN_ALT_M (-500.0f)
#define ASCENT_MAX_ALT_M (9000.0f)

// Faster than this vertically and the sample is a spike, not a rider. Well
// above what anyone climbs and above a fast descent too, so it rejects only
// what is plainly wrong.
#define ASCENT_MAX_RATE_MPS (6.0f)

// Samples further apart than this are not a rate at all -- the receiver lost
// its fix, or the rider paused the ride -- so the rate test is skipped and the
// hysteresis alone decides.
#define ASCENT_MAX_GAP_MS 30000u

// Exponential smoothing on the accepted altitude. Low enough to flatten
// per-sample wander, high enough that a genuine climb is not lagged into the
// next postcode.
#define ASCENT_SMOOTH_ALPHA 0.25f

// The hysteresis band, in metres. Roughly the noise a good fix shows, so
// wander stays inside it and a real hill does not.
#define ASCENT_BAND_M 4.0f

// ---------------------------------------------------------------------------
// The same filters, retuned for a barometer
// ---------------------------------------------------------------------------
// Every constant above is sized against GNSS noise, and feeding a barometer
// through them wastes the sensor: a 4m band discards every climb smaller than
// itself, which is most of what a barometer can see and a receiver cannot.
// A BMP580 resolves centimetres of change, and hal/Barometer.cpp has already
// smoothed it once before it reaches here.
//
// 1m is the band a barometric bike computer conventionally uses -- above the
// part's noise, below any hill worth counting.
#define ASCENT_BARO_BAND_M 1.0f

// Lighter smoothing, because the driver has already applied its own and
// stacking two lags a real climb without removing anything more.
#define ASCENT_BARO_SMOOTH_ALPHA 0.50f

// ⚠️ What neither band fixes: a barometer cannot tell a weather system from a
// hill. A front passing during a long ride moves the pressure steadily, and
// a slow steady rise is exactly what a climb looks like. The band rejects
// noise, not drift. Real head units live with this; it is worth knowing
// before trusting a climb total from a four-hour ride in changing weather.
typedef enum {
    ASCENT_SOURCE_GNSS = 0,
    ASCENT_SOURCE_BARO = 1,
} AscentSource_t;

typedef struct {
    bool has_smooth;
    float smooth_m; // exponentially smoothed altitude

    bool has_raw;
    float last_raw_m; // for the rate test
    uint32_t last_ms;

    bool has_ref;
    float ref_m; // the hysteresis reference

    double ascent_m;
    double descent_m;

    // Tuning, carried per-accumulator rather than read from the macros, so
    // the same filter serves both sources. Set by Ascent_ResetFor().
    float band_m;
    float alpha;
} Ascent_t;

// Zeroes everything and tunes for GNSS. Call before the first sample and at
// the start of a ride.
void Ascent_Reset(Ascent_t *a);

// The same, tuned for a named source. Ascent_Reset() is this with
// ASCENT_SOURCE_GNSS, kept because most callers and every existing test want
// exactly that.
void Ascent_ResetFor(Ascent_t *a, AscentSource_t source);

// Offers one altitude, stamped with the time it arrived. Returns true if the
// sample was accepted, which is useful to tests and to nothing else.
//
// `time_ms` may wrap; the gap is computed with unsigned arithmetic.
bool Ascent_Feed(Ascent_t *a, float alt_m, uint32_t time_ms);

// Metres climbed and metres dropped so far. Both are positive.
double Ascent_Metres(const Ascent_t *a);
double Ascent_DescentMetres(const Ascent_t *a);

#ifdef __cplusplus
}
#endif
