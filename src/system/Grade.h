#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Road grade, as a percentage of rise over run, from barometric altitude and
// horizontal distance.
//
// Pure arithmetic and no hardware: hal/Barometer.h supplies the altitude,
// GradeTracker.h integrates the distance, and test/host/test_grade.c is where
// this is actually checked.
//
// ---------------------------------------------------------------------------
// Why not the IMU, which is what the spec asked for
// ---------------------------------------------------------------------------
// An accelerometer cannot measure grade on a bicycle. It measures specific
// force, and it has no way to separate gravity from the rider's own
// acceleration: pulling away from a light at a gentle 1 m/s^2 tilts the
// apparent vertical by atan(1/9.81) = 5.8 degrees, which reads as a **10%
// climb on flat ground**. Mounting angle on the handlebars adds a fixed
// offset that needs zeroing, and road buzz adds noise on top of both.
//
// Rise over run has none of those failure modes. It is also what every
// commercial head unit does, for the same reason.
//
// The IMU is still worth having here eventually, but as the *fast* half of a
// pair rather than the source: gyro pitch integrated between barometric
// updates, with the figure below as its drift reference. That buys back the
// lag described next. It is not needed for the number to be correct.
//
// ---------------------------------------------------------------------------
// What sets GRADE_MIN_RUN_M
// ---------------------------------------------------------------------------
// Grade is a ratio of two small numbers, so it is the *run* that decides how
// much altitude error shows up in the answer. Near sea level 1 m of altitude
// is about 12 Pa, and the BMP580 with BaroAltitude's filter in front of it
// settles to well under a tenth of a metre -- so over 30 m of run, sensor
// noise alone is worth about 0.3% of grade. Fine.
//
// The error that actually matters is **pressure, not noise**. A gust, a
// passing lorry, or a case vented into the airstream shifts the reading
// bodily: 10 m/s of dynamic pressure is 0.5*1.2*10^2 = 60 Pa, which is 5 m of
// apparent altitude. Over a 30 m run that is an entire 17% of invented grade.
// Nothing in this file can fix that -- it is a mounting question -- but it is
// why the run threshold is tens of metres rather than a few, why there is an
// output filter below, and why the figure is clamped.
//
// The cost of all three is lag: at 18 km/h, 30 m is six seconds, and the
// filter adds a few more. The number is right and it is late. That is the
// correct trade for a climb readout and the wrong one for, say, a shift
// recommendation.

// Horizontal run required before a grade is published at all. Below this the
// altitude terms above swamp the ratio.
#define GRADE_MIN_RUN_M 30.0f

// How far back the window may reach. Older than this and the far end of the
// baseline is terrain the rider has left; it also bounds how long a grade
// survives once the rider stops (see Grade_Feed).
#define GRADE_MAX_WINDOW_MS 30000u

// Clamp. Steeper than any road surface, so anything beyond it is a pressure
// artefact. Clamped rather than rejected: a bounded wrong number stays
// readable and recovers, where a blanked cell on a genuine 25% wall is just
// missing.
#define GRADE_MAX_PCT 30.0f

// Output filter weight, applied to each new estimate. The first estimate is
// taken whole, so the figure does not ramp up from zero when it appears.
#define GRADE_SMOOTH_ALPHA 0.30f

// History depth. The effective window is whichever of this and
// GRADE_MAX_WINDOW_MS runs out first -- at the ATGM336H's 1Hz this is a
// minute of samples and the time bound always wins, but a 5Hz receiver would
// hit this one first and shorten the window at low speed.
#define GRADE_SAMPLES 64

typedef struct {
    double dist_m; // cumulative horizontal distance, monotonic
    float alt_m;
    uint32_t t_ms;
} GradeSample_t;

typedef struct {
    GradeSample_t s[GRADE_SAMPLES];
    uint8_t oldest; // ring index of the oldest live sample
    uint8_t count;
    float grade_pct;
    bool have_grade;
} Grade_t;

void Grade_Reset(Grade_t *g);

// Folds in one (distance, altitude) sample and returns whether a grade is
// available afterwards.
//
// `dist_m` is cumulative and must not go backwards; one that does is read as
// an odometer reset and clears the history rather than producing a negative
// run. `t_ms` is a free-running millisecond tick and may wrap.
//
// A rider who stops keeps the last real grade until the window empties of
// samples from before the stop -- which is deliberate, because someone
// halted on a hill is still on that hill -- and then loses it. Every live
// reading on this panel ages out (CLAUDE.md §8); this is that rule.
bool Grade_Feed(Grade_t *g, double dist_m, float alt_m, uint32_t t_ms);

bool Grade_Have(const Grade_t *g);

// Percent, signed: positive uphill. Meaningless unless Grade_Have().
float Grade_Pct(const Grade_t *g);

#ifdef __cplusplus
}
#endif
