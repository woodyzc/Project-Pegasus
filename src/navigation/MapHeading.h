#pragma once

#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

// Which way is "up" when the map is drawn track-up.
//
// Not simply the receiver's heading. Two things make the raw value unusable
// for rotating a screen:
//
//   * Standing still, it is meaningless. A stationary receiver reports the
//     direction of its own noise, which swings through the full circle in
//     seconds. A map rotating on that at a traffic light is worse than no
//     rotation at all.
//   * Riding, it is jittery at the degree level. Rotating a map by two
//     degrees forty times a minute costs a redraw each time and looks like a
//     tremor.
//
// So this holds the last good heading below a speed floor, smooths what it
// accepts above it, and reports whether the result moved far enough to be
// worth redrawing. Pure, and host-tested, because every one of those rules is
// about a sequence -- and because the wrap from 359 to 0 is where a smoother
// written the obvious way spins the map the long way round.

#ifdef __cplusplus
extern "C" {
#endif

// Below this the heading is held rather than updated. Roughly walking pace,
// and the same order as TripAccum's distance floor, so the two agree about
// when a rider is moving.
#define MAP_HEADING_MIN_MPS 1.5f

// Exponential smoothing on the accepted heading. Low enough to settle the
// jitter, high enough that the map has finished turning by the time the rider
// has.
#define MAP_HEADING_ALPHA 0.25f

// How far the smoothed heading must move before a redraw is worth it. Below
// this the rotation is invisible and the frame is spent for nothing.
#define MAP_HEADING_REDRAW_DEG 2.0f

typedef struct {
    bool valid;
    float heading_deg;  // smoothed, 0 <= h < 360
    float drawn_deg;    // what the map was last drawn at
    bool has_drawn;
} MapHeading_t;

void MapHeading_Reset(MapHeading_t *h);

// Offers one fix. Returns true when the map should be redrawn -- either the
// first usable heading, or a change past MAP_HEADING_REDRAW_DEG since the last
// redraw. Calling it and ignoring the result is safe; the heading still
// updates.
bool MapHeading_Feed(MapHeading_t *h, bool fix_valid, float speed_mps, float heading_deg);

// The angle to rotate the map by, and whether there is one yet. Without a
// heading the map stays north-up, which is the honest default: a device that
// has never moved does not know which way it is pointing.
bool MapHeading_Valid(const MapHeading_t *h);
float MapHeading_Degrees(const MapHeading_t *h);

// Signed difference a-b, wrapped to (-180, 180]. Exposed because it is the
// part that is easy to get wrong and worth testing directly.
float MapHeading_Delta(float a, float b);

#ifdef __cplusplus
}
#endif
