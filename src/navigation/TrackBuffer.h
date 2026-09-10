#pragma once

#include <stdint.h>
#include <stddef.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

// Fixed-capacity store for a breadcrumb trail, filled from a GPX file.
//
// The problem it solves: a recorded ride routinely holds tens of thousands of
// track points, the file size is not known until it has been read, and the
// screen is 240x320. Loading everything and thinning afterwards would need the
// whole file in memory first, which is exactly what streaming the parse was
// meant to avoid.
//
// So it thins as it fills. When the buffer runs out of room it keeps every
// second point and doubles its stride, then carries on. The result is an
// evenly sampled trail of bounded size from a single pass over a file of
// unknown length -- the shape survives, only the density drops.
//
// Coordinates are stored as degrees x 1e-7 in int32, the same representation
// UBX uses. That is ~11mm of resolution, exact, and 8 bytes per point rather
// than the 16 a pair of doubles would cost -- worth it at 20k points.
//
// Pure: no Arduino, no SD, so test/host covers the thinning and the bounds.

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int32_t *lat_e7; /* caller-owned storage, `capacity` entries each */
    int32_t *lon_e7;
    size_t capacity;
    size_t count;

    uint32_t stride;  /* accept one point in every `stride` */
    uint32_t skipped; /* position within the current stride */

    /* Bounds cover every point OFFERED, not just those kept, so auto-fitting
       the view frames the real trail rather than the thinned sample. */
    bool has_bounds;
    int32_t min_lat_e7;
    int32_t max_lat_e7;
    int32_t min_lon_e7;
    int32_t max_lon_e7;
} TrackBuffer_t;

// `lat_store` and `lon_store` must each hold `capacity` int32_t.
void TrackBuffer_Init(TrackBuffer_t *track, int32_t *lat_store, int32_t *lon_store,
                      size_t capacity);

// Offers one point. Always updates the bounds; stores it only if the current
// stride selects it. Halves the contents and doubles the stride when full.
void TrackBuffer_Add(TrackBuffer_t *track, double lat_deg, double lon_deg);

// Reads back a stored point in degrees. False if `index` is out of range.
bool TrackBuffer_Get(const TrackBuffer_t *track, size_t index, double *out_lat, double *out_lon);

// Centre of the bounding box, for framing the view. False if nothing added.
bool TrackBuffer_Center(const TrackBuffer_t *track, double *out_lat, double *out_lon);

#ifdef __cplusplus
}
#endif
