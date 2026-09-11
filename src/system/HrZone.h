#pragma once

#include <stdint.h>
#include <stddef.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

// Five heart-rate training zones by heart-rate reserve, kept free of LVGL so
// the arithmetic can be tested on the host.
//
// Reserve rather than percentage of maximum: HRR measures effort from where
// the rider actually sits at rest, so a zone means the same thing regardless
// of resting rate.
//
//     target = rest + fraction * (max - rest)
//
// ---------------------------------------------------------------------------
// The band edges are NOT the textbook 50/60/70/80/90
// ---------------------------------------------------------------------------
// They are 30/40/60/90, taken from the rider's own Samsung Health zones so the
// head unit and the phone agree about what "zone 4" means. Verified against a
// screenshot of that app at rest 51 / max 174:
//
//     zone 1   51-87     rest .. 30% HRR    low intensity
//     zone 2   88-100    30 .. 40%          weight control
//     zone 3   101-124   40 .. 60%          aerobic
//     zone 4   125-161   60 .. 90%          anaerobic
//     zone 5   162-174   90 .. 100%         maximum
//
// The bands are deliberately unequal -- zones 2 and 5 span 13bpm while 1 and 4
// span 37 -- which is why the UI sizes each segment to its own bpm span. Equal
// segments with a linear marker would disagree with their own boundaries.
// ---------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

#define HR_ZONE_COUNT 5

// Upper edge of each zone as a fraction of reserve. Index i is the top of zone
// i, so the bottom of zone i is the top of zone i-1 (and rest for zone 0).
extern const double HR_ZONE_UPPER_FRACTION[HR_ZONE_COUNT];

// Zone for `bpm`, 0..HR_ZONE_COUNT-1. Anything at or below rest reports zone 0
// and anything at or above max reports zone 4. A nonsensical pair (max at or
// below rest) yields zone 0 rather than dividing by zero.
int HrZone_Index(uint8_t bpm, uint8_t rest_bpm, uint8_t max_bpm);

// Where `bpm` sits along the whole bar: 0.0 at rest, 1.0 at max, clamped.
// Linear in bpm, which is what lets a marker drawn from it agree with segments
// sized by HrZone_SpanFraction().
double HrZone_Fraction(uint8_t bpm, uint8_t rest_bpm, uint8_t max_bpm);

// Width of `zone` as a fraction of the whole bar, if the bar is drawn with
// each zone sized to the beats it actually spans. The five sum to 1.0.
double HrZone_SpanFraction(int zone);

// Where `bpm` sits along a bar whose five zones are all drawn the SAME width,
// 0.0 at rest and 1.0 at max.
//
// This exists because the two ways of drawing the bar need different maths,
// and mixing them puts the marker in the wrong place. HrZone_Fraction is
// linear in bpm, which is right when each segment is as wide as its own span.
// Give every zone a fifth of the bar instead and that breaks: zone 4 covers
// 30% of the reserve but only 20% of the bar, so a marker placed by reserve
// drifts out of the segment that is lit. This maps the reading to its zone
// first and then to the position within it, so marker and lit segment always
// agree -- at the cost of the marker moving at different speeds per zone,
// which is the honest trade for equal segments.
double HrZone_EqualWidthFraction(uint8_t bpm, uint8_t rest_bpm, uint8_t max_bpm);

// Lowest and highest bpm in `zone`, matching what the phone displays: the
// lower edge is one beat above the previous zone's upper edge, so the bands
// are contiguous without overlapping.
uint8_t HrZone_LowerBpm(int zone, uint8_t rest_bpm, uint8_t max_bpm);
uint8_t HrZone_UpperBpm(int zone, uint8_t rest_bpm, uint8_t max_bpm);

#ifdef __cplusplus
}
#endif
