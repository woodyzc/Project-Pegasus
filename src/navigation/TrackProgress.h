#pragma once

#include <stddef.h>
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#include "TrackBuffer.h"

// How far along a loaded GPX track the rider has got -- the mark that splits
// the map's trail into grey (ridden) and magenta (still to ride).
//
// The mark only moves forward, so a rider who turns round does not see the
// stretch they already covered go magenta again behind them. That makes a
// wrong advance PERMANENT, and the first version of this made wrong advances
// routinely:
//
// ---------------------------------------------------------------------------
// WHY NOT "NEAREST VERTEX ANYWHERE ON THE TRACK"
// ---------------------------------------------------------------------------
// That is what it was: the nearest track point to the fix, searched over the
// whole track, accepted if within 200m, and max()'d into the mark. On a real
// route it jumps:
//
//   * Any later stretch that passes near the rider -- the return leg of an
//     out-and-back, a road ridden twice, a parallel street in a grid -- can
//     hold a vertex closer than the nearest one on the stretch actually being
//     ridden. A planned route on a straight road has vertices hundreds of
//     metres apart, so "closer" is easy. The mark jumps to that later stretch
//     and everything up to it goes grey at once.
//   * Because the mark cannot go back, riding on (or turning round) never
//     brings the magenta back. Seen on the road 2026-10-09 as "the highlighted
//     route vanishes for no reason, and riding the other way does not bring it
//     back".
//
// It is the map's copy of the failure CLAUDE.md section 5 records for
// RouteFollow, and the cure is the same shape: measure to SEGMENTS rather than
// vertices, and use where the rider already was.
//
// ---------------------------------------------------------------------------
// WHAT THIS DOES INSTEAD
// ---------------------------------------------------------------------------
//   * Tracking: once there is a mark, only the next TRACK_PROGRESS_WINDOW_M of
//     route ahead of it is searched. A later leg that happens to pass nearby is
//     simply not a candidate. Within the window, a small cost per metre ahead
//     breaks the tie an out-and-back's turnaround produces in favour of the
//     nearer pass.
//   * (Re)acquiring: with no mark, or with the rider somewhere past the window
//     (a gap in fixes, a shortcut, the map page left closed for ten minutes),
//     the rest of the route is searched -- but a candidate there must hold for
//     TRACK_PROGRESS_ACQUIRE_FIXES fixes in a row, each consistent with the
//     last, and the rider must have moved FORWARD along the route over them,
//     before the mark moves. One stray fix that lands near some other part of
//     the route cannot grey out everything before it, and neither can riding
//     the route in reverse -- which, measured by position alone, would grey
//     the whole of it from the far end on the first fix.
//   * Where the route covers the same ground twice, the (re)acquiring search
//     only considers segments running the way the rider is going. Without
//     that, a rider picked up on the way home of an out-and-back was matched
//     to the outbound leg -- the earlier index -- and the forward check then
//     failed on every fix, so they were never picked up at all.
//
// Pure, and host-tested (test/host/test_track_progress.c), because every rule
// here is about a sequence of fixes.

#ifdef __cplusplus
extern "C" {
#endif

// Cross-track distance within which a fix counts as ON the track. Measured to
// the segment, not the nearest vertex, which is why this can be a quarter of
// the old 200m: vertex spacing no longer inflates it. Generous enough for a
// path beside the road centreline OSM recorded plus a receiver under trees --
// the same reasoning as ROUTE_OFF_ROUTE_M.
#define TRACK_PROGRESS_ONTRACK_M 50.0f

// How much route ahead of the mark the tracking search covers. Between two
// fixes a rider covers ~10m; this is far beyond that so a few dropped fixes do
// not lose the lock, and far short of the length at which an unrelated stretch
// of a typical route comes back past the rider.
#define TRACK_PROGRESS_WINDOW_M 600.0f

// Score = cross-track + this x metres ahead of the mark. At an out-and-back's
// turnaround both legs are on the line; 0.1 makes the nearer one win by 10m
// per 100m of separation, and is too small to beat a real cross-track
// difference on an ordinary route.
#define TRACK_PROGRESS_AHEAD_COST 0.1f

// Outside the window, a candidate must hold this many consecutive fixes...
#define TRACK_PROGRESS_ACQUIRE_FIXES 5
// ...each within this distance of the previous candidate and not behind it...
#define TRACK_PROGRESS_ACQUIRE_STEP_M 150.0f
// ...and between the first and the last of them the rider must have moved at
// least this far in the route's direction. Riding it backwards, or crossing it
// at a junction, never satisfies this. Four seconds at a slow 10 km/h is 11m.
#define TRACK_PROGRESS_ACQUIRE_FORWARD_M 10.0f

// In the out-of-window search, a later segment must be closer than an earlier
// one by this much to win -- so where two passes of the route are both on the
// line, the earlier is assumed. It is the honest default: it greys out less.
#define TRACK_PROGRESS_TIE_M 10.0f

// The step between two fixes that counts as a direction of travel. Below it --
// a rider stopped, or walking the bike -- the step is mostly receiver wander,
// and the out-of-window search goes by position alone. Two metres in a second
// is 7 km/h.
#define TRACK_PROGRESS_MOTION_MIN_M 2.0f

typedef struct {
    // The last track VERTEX passed: vertices [0, mark] are ridden. It is the
    // start of the segment the rider last snapped to.
    size_t mark;
    bool have_mark;

    // Evidence for a jump the window cannot see: the candidate segment, how
    // many fixes in a row have supported it, and where the first of them was.
    size_t pending;
    uint8_t pending_count;
    int32_t pending_from_lat_e7;
    int32_t pending_from_lon_e7;

    // The previous fix, for the direction of travel.
    int32_t last_lat_e7;
    int32_t last_lon_e7;
    bool have_last;
} TrackProgress_t;

void TrackProgress_Reset(TrackProgress_t *p);

// Offers one valid fix, as 1e-7 degrees (TrackBuffer's own units). Returns
// true when the mark moved. Never moves it backwards.
bool TrackProgress_Feed(TrackProgress_t *p, const TrackBuffer_t *track, int32_t lat_e7,
                        int32_t lon_e7);

#ifdef __cplusplus
}
#endif
