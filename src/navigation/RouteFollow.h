#pragma once

#include <stdint.h>
#include <stddef.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#include "RouteParse.h"

// Following a cached route with nothing but our own GPS fix.
//
// This is what turns a downloaded route into navigation after the phone is
// gone. Given a position it answers two questions: how far along the route are
// we, and which maneuver comes next. The live BLE directive (TbtParse.h) is
// preferred whenever the phone is connected, because the phone has map
// matching and we do not -- this is the fallback, and it has to be good enough
// that a dropped link is a non-event.
//
// Pure geometry, no allocation, no Arduino, so test/host covers it. The PSRAM
// buffers and the "which source is live" decision are in NavRoute.h.
//
// ---------------------------------------------------------------------------
// WHY SNAPPING, NOT "NEAREST MANEUVER"
// ---------------------------------------------------------------------------
// The obvious cheap version picks whichever maneuver is nearest in a straight
// line. It fails on exactly the geometry a bike route is full of: an
// out-and-back, a switchback, a loop that passes under itself. The rider is
// 20m from a maneuver they will not reach for another 8km, and the display
// says turn.
//
// Snapping to the polyline and measuring ALONG it does not have that failure.
// Two passes of the same road are two different distances along the route even
// though they are the same place on the ground.

// How far off the line before we stop believing we are on the route.
//
// Generous on purpose. A bike path beside a road is often 15m from the
// centreline OSM recorded, and a consumer GPS under trees is happily another
// 15m out. Declaring off-route too eagerly would blank a perfectly good route
// under every bridge. Going genuinely off-route in v1 only stops the distance
// countdown updating -- there is no rerouting -- so the cost of being late to
// notice is much lower than the cost of crying wolf.
#define ROUTE_OFF_ROUTE_M 50

typedef struct {
    uint16_t segment_index;    // polyline segment the fix snapped to
    uint32_t distance_along_m; // from the route start, along the line
    uint32_t cross_track_m;    // perpendicular distance from the line
    bool off_route;            // cross_track_m > ROUTE_OFF_ROUTE_M
} RouteFix_t;

#ifdef __cplusplus
extern "C" {
#endif

// Fills `out_cum` with the cumulative distance in metres at each point, so
// `out_cum[0]` is 0 and the last entry is the route length. `out_cum` must
// hold manifest->point_count entries. Returns the total length.
//
// Computed once when a route is stored rather than per fix: it is the same
// answer every time, and a fix arrives every second for hours.
uint32_t RouteFollow_BuildCumulative(const uint8_t *blob,
                                     const RouteManifest_t *manifest,
                                     uint32_t *out_cum);

// Snaps a position to the route, with no idea where the rider was. False only
// if the inputs are unusable.
//
// Searches every segment. That is O(n) per fix -- about 4,000 segments worst
// case, once a second -- which measured far cheaper than the bookkeeping a
// windowed search around the last fix would need, and unlike a windowed search
// it recovers immediately from a GPS jump, a paused ride, or a head unit
// restarted mid-route.
//
// Use this only when there is genuinely no previous fix. Snapping on geometry
// alone cannot settle an out-and-back -- see RouteFollow_SnapFrom.
bool RouteFollow_Snap(const uint8_t *blob,
                      const RouteManifest_t *manifest,
                      const uint32_t *cum,
                      double lat,
                      double lon,
                      RouteFix_t *out);

// The same snap, told where the rider was on the previous fix.
//
// ---------------------------------------------------------------------------
// WHY THE SNAP NEEDS A MEMORY
// ---------------------------------------------------------------------------
// Snapping to the polyline fixes the "nearest maneuver" failure described
// above, and it does not fix the one underneath it. On an out-and-back the
// two legs are the same points in the same order, so both segments snap with
// the same cross-track -- to within floating point when the route repeats the
// same geometry, which is exactly what a planner returns for a there-and-back.
// The global minimum then settles it by segment index, which always means the
// outbound leg. A rider on the way home gets the outbound leg's next turn for
// the whole return: wrong arrow, wrong street, wrong countdown, and it never
// self-corrects because every fix re-decides it the same way.
//
// So the tie is broken by where the rider already was. This is NOT a windowed
// search: every segment is still examined, and the hint can only discount a
// candidate by ROUTE_SNAP_HINT_BUDGET_M. A fix that genuinely belongs
// elsewhere on the route beats a stale hint outright, which is what keeps the
// GPS-jump and resumed-ride recovery the unhinted search promises above.
//
// `have_hint` false is identical to RouteFollow_Snap, and that is the right
// call after a restart, a long gap, or any other break where the last known
// position stopped meaning anything.
bool RouteFollow_SnapFrom(const uint8_t *blob,
                          const RouteManifest_t *manifest,
                          const uint32_t *cum,
                          double lat,
                          double lon,
                          bool have_hint,
                          uint32_t hint_along_m,
                          RouteFix_t *out);

// True when the blob's maneuvers are in the ascending distance order every
// function below depends on. Check it once, when a route finishes arriving.
//
// The linear scan in RouteFollow_NextManeuver returns the FIRST entry at or
// beyond the rider, so an out-of-order array does not fail loudly -- it
// silently returns a maneuver the rider has already ridden past, for the rest
// of the route, with a confident distance attached. The phone encoder emits
// them in order and Route_ParseManifest has no opinion on it, so this is the
// only place the assumption can be turned into a fact. It is O(maneuver_count)
// once per route against a ceiling of 256.
//
// Equal distances are allowed: two instructions at one coordinate is a real
// thing a router emits, and it is only strict inversion that breaks the scan.
bool RouteFollow_ManeuversOrdered(const uint8_t *blob, const RouteManifest_t *manifest);

// The first maneuver at or beyond `distance_along_m`, and how far away it is.
// False when the route has no maneuvers left, which is the arrival case.
//
// Maneuvers must be in ascending distance order. The phone encoder guarantees
// that, Route_ParseManifest does not check it, and NavRoute refuses a route
// that fails RouteFollow_ManeuversOrdered() above rather than navigating it
// badly in silence.
bool RouteFollow_NextManeuver(const uint8_t *blob,
                              const RouteManifest_t *manifest,
                              uint32_t distance_along_m,
                              RouteManeuver_t *out_maneuver,
                              uint32_t *out_distance_to_m);

// Metres between two 1e7-degree coordinates, equirectangular. Exposed because
// the tests need to state expectations in the same approximation the
// implementation uses, rather than restating the formula and drifting.
double RouteFollow_MetresBetween(int32_t lat_a, int32_t lon_a, int32_t lat_b, int32_t lon_b);

#ifdef __cplusplus
}
#endif
