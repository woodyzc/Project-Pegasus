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

// Snaps a position to the route. False only if the inputs are unusable.
//
// Searches every segment. That is O(n) per fix -- about 4,000 segments worst
// case, once a second -- which measured far cheaper than the bookkeeping a
// windowed search around the last fix would need, and unlike a windowed search
// it recovers immediately from a GPS jump, a paused ride, or a head unit
// restarted mid-route.
bool RouteFollow_Snap(const uint8_t *blob,
                      const RouteManifest_t *manifest,
                      const uint32_t *cum,
                      double lat,
                      double lon,
                      RouteFix_t *out);

// The first maneuver at or beyond `distance_along_m`, and how far away it is.
// False when the route has no maneuvers left, which is the arrival case.
//
// Maneuvers must be in ascending distance order; the phone encoder guarantees
// that and Route_ParseManifest does not check it, because a route that
// violated it would still navigate, just badly.
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
