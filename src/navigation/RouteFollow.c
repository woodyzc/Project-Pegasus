#include "RouteFollow.h"

#include <math.h>

#include "MapProject.h" // MAP_EARTH_METRES_PER_DEGREE, shared with the map layers

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Degrees of longitude shrink towards the poles; degrees of latitude do not.
// Everything here is a local measurement over at most a few hundred metres, so
// one cosine at the local latitude is accurate to far better than GPS noise
// and avoids a haversine per segment per fix.
static double LonScale(double lat_deg) {
    const double c = cos(lat_deg * M_PI / 180.0);
    // Clamped away from zero so a fix at a pole cannot divide the world by
    // nothing. Nobody will ride there, but a garbage fix can claim to.
    return c > 0.01 ? c : 0.01;
}

double RouteFollow_MetresBetween(int32_t lat_a, int32_t lon_a, int32_t lat_b, int32_t lon_b) {
    const double lat_a_deg = (double)lat_a / ROUTE_COORD_SCALE;
    const double lat_b_deg = (double)lat_b / ROUTE_COORD_SCALE;
    const double lon_a_deg = (double)lon_a / ROUTE_COORD_SCALE;
    const double lon_b_deg = (double)lon_b / ROUTE_COORD_SCALE;

    const double mid_lat = (lat_a_deg + lat_b_deg) * 0.5;
    const double dy = (lat_b_deg - lat_a_deg) * MAP_EARTH_METRES_PER_DEGREE;
    const double dx = (lon_b_deg - lon_a_deg) * MAP_EARTH_METRES_PER_DEGREE * LonScale(mid_lat);
    return sqrt(dx * dx + dy * dy);
}

uint32_t RouteFollow_BuildCumulative(const uint8_t *blob,
                                     const RouteManifest_t *manifest,
                                     uint32_t *out_cum) {
    if (blob == NULL || manifest == NULL || out_cum == NULL || manifest->point_count < 2) {
        return 0;
    }

    // Accumulated as a double and rounded once per point. Summing rounded
    // metres instead would drift: consumer GPS traces have points a few metres
    // apart, and rounding each of several thousand of them the same way biases
    // the total.
    double running = 0.0;
    out_cum[0] = 0;

    RoutePoint_t prev;
    if (!Route_Point(blob, manifest, 0, &prev)) {
        return 0;
    }

    for (uint16_t i = 1; i < manifest->point_count; i++) {
        RoutePoint_t cur;
        if (!Route_Point(blob, manifest, i, &cur)) {
            return 0;
        }
        running += RouteFollow_MetresBetween(prev.lat, prev.lon, cur.lat, cur.lon);
        out_cum[i] = (uint32_t)(running + 0.5);
        prev = cur;
    }
    return out_cum[manifest->point_count - 1];
}

// The most a hint can move the decision, in metres of cross-track.
//
// This bound is the whole design. Two passes of the same road are the same
// place on the ground: both legs snap with the same cross-track, and no
// amount of geometry can choose between them. Where the rider already was
// can, and does.
//
// But a hint is not evidence. Capping its influence means it only ever breaks
// a near-tie -- a fix that genuinely belongs somewhere else on the route is
// further than this from the stale candidate and wins outright anyway. That
// is what preserves the property the unhinted search promises: a GPS jump, a
// tunnel, a ride resumed miles away all re-acquire on the very next fix
// instead of being dragged back to a hint that stopped being true.
#define ROUTE_SNAP_HINT_BUDGET_M 25.0

// What a metre of claimed movement costs, forward and backward.
//
// Both directions have to be priced, and the first attempt at this made
// forward free -- which fails on precisely the geometry it was written for.
// The return leg's candidate is ALWAYS ahead of the hint (that is what a
// return leg is), so a free forward direction never charges it anything, and
// the whole decision falls back to the coin-flip between two identical
// cross-tracks. The test that walks a ride through a turnaround catches this;
// a single-fix test does not.
//
// Forward is cheap rather than free: a rider covers metres between fixes and
// must not be billed for it, but a candidate a hundred and seventy metres
// further along than one second ago is not where they went. Backward is dear,
// because riding back down the route is the thing that does not happen -- and
// it has to outweigh the forward charge at the turnaround, where the honest
// answer IS a jump forward (to the mirrored distance) and the wrong answer is
// a small step back.
#define ROUTE_SNAP_FORWARD_COST 0.5
#define ROUTE_SNAP_BACKWARD_COST 3.0

// Scores below carry the fix's real distance from the line plus this; two
// candidates within a twentieth of a metre of each other are the same answer
// as far as any GPS is concerned, and the tie is then settled by taking the
// earlier one rather than by whichever way the last bit of a double fell.
//
// Without it the start of a there-and-back is a coin flip: at the shared
// start-and-finish point both legs score identically, and landing on the
// return leg means opening the ride already arrived.
#define ROUTE_SNAP_TIE_M 0.05

// How implausible a candidate is, in metres, given where the rider last was.
static double HintPenalty(double along, double hint_along) {
    const double delta = along - hint_along;
    const double cost = (delta >= 0.0) ? delta * ROUTE_SNAP_FORWARD_COST
                                       : -delta * ROUTE_SNAP_BACKWARD_COST;
    return cost < ROUTE_SNAP_HINT_BUDGET_M ? cost : ROUTE_SNAP_HINT_BUDGET_M;
}

bool RouteFollow_Snap(const uint8_t *blob,
                      const RouteManifest_t *manifest,
                      const uint32_t *cum,
                      double lat,
                      double lon,
                      RouteFix_t *out) {
    return RouteFollow_SnapFrom(blob, manifest, cum, lat, lon, false, 0, out);
}

bool RouteFollow_SnapFrom(const uint8_t *blob,
                          const RouteManifest_t *manifest,
                          const uint32_t *cum,
                          double lat,
                          double lon,
                          bool have_hint,
                          uint32_t hint_along_m,
                          RouteFix_t *out) {
    if (blob == NULL || manifest == NULL || cum == NULL || out == NULL ||
        manifest->point_count < 2) {
        return false;
    }

    // Work in metres on a plane whose origin is the fix. Over one segment the
    // distortion is immaterial, and it keeps the arithmetic in a range where
    // double has far more precision than the measurement deserves.
    const double lon_scale = LonScale(lat);

    // The winner is chosen on `score`, which is the cross-track plus whatever
    // the hint charges it; `best_cross` is the winner's real distance from the
    // line, and it is what gets reported and what decides off_route. Scoring
    // and measuring have to stay separate -- a candidate that won by 20m of
    // hint discount is still exactly as far off the road as it was.
    double best_score = INFINITY;
    double best_cross = INFINITY;
    uint16_t best_segment = 0;
    double best_along = 0.0;
    const double hint = (double)hint_along_m;

    RoutePoint_t a;
    if (!Route_Point(blob, manifest, 0, &a)) {
        return false;
    }

    for (uint16_t i = 0; i + 1 < manifest->point_count; i++) {
        RoutePoint_t b;
        if (!Route_Point(blob, manifest, (uint16_t)(i + 1), &b)) {
            return false;
        }

        const double ax = ((double)a.lon / ROUTE_COORD_SCALE - lon) *
                          MAP_EARTH_METRES_PER_DEGREE * lon_scale;
        const double ay = ((double)a.lat / ROUTE_COORD_SCALE - lat) *
                          MAP_EARTH_METRES_PER_DEGREE;
        const double bx = ((double)b.lon / ROUTE_COORD_SCALE - lon) *
                          MAP_EARTH_METRES_PER_DEGREE * lon_scale;
        const double by = ((double)b.lat / ROUTE_COORD_SCALE - lat) *
                          MAP_EARTH_METRES_PER_DEGREE;

        const double abx = bx - ax;
        const double aby = by - ay;
        const double len_sq = abx * abx + aby * aby;

        // Where along AB the perpendicular from the fix lands, clamped to the
        // segment so a fix beyond either end measures to the endpoint rather
        // than to a point on the infinite line that is not on the route.
        double t = 0.0;
        if (len_sq > 1e-9) {
            t = -(ax * abx + ay * aby) / len_sq;
            if (t < 0.0) {
                t = 0.0;
            } else if (t > 1.0) {
                t = 1.0;
            }
        }

        const double cx = ax + t * abx;
        const double cy = ay + t * aby;
        const double cross = sqrt(cx * cx + cy * cy);

        // Interpolate within the segment using the cumulative table, so the
        // answer stays consistent with the length the table reports even where
        // the two formulas would differ slightly. Needed before the comparison
        // now rather than after it, because the hint prices this distance.
        const double seg_len = (double)cum[i + 1] - (double)cum[i];
        const double along = (double)cum[i] + t * seg_len;

        const double score = have_hint ? cross + HintPenalty(along, hint) : cross;

        if (score < best_score - ROUTE_SNAP_TIE_M) {
            best_score = score;
            best_cross = cross;
            best_segment = i;
            best_along = along;
        } else if (score < best_score + ROUTE_SNAP_TIE_M && along < best_along) {
            // Indistinguishable from the leader; take the earlier one. Only
            // the true minimum is ever kept in best_score, so a run of
            // near-ties cannot ratchet the threshold upwards.
            if (score < best_score) {
                best_score = score;
            }
            best_cross = cross;
            best_segment = i;
            best_along = along;
        }

        a = b;
    }

    if (!isfinite(best_score)) {
        return false;
    }

    out->segment_index = best_segment;
    out->distance_along_m = (uint32_t)(best_along + 0.5);
    out->cross_track_m = (uint32_t)(best_cross + 0.5);
    out->off_route = best_cross > (double)ROUTE_OFF_ROUTE_M;
    return true;
}

bool RouteFollow_NextManeuver(const uint8_t *blob,
                              const RouteManifest_t *manifest,
                              uint32_t distance_along_m,
                              RouteManeuver_t *out_maneuver,
                              uint32_t *out_distance_to_m) {
    if (blob == NULL || manifest == NULL || out_maneuver == NULL || out_distance_to_m == NULL) {
        return false;
    }

    for (uint16_t i = 0; i < manifest->maneuver_count; i++) {
        RouteManeuver_t m;
        if (!Route_Maneuver(blob, manifest, i, &m)) {
            return false;
        }
        if (m.distance_along_route_m >= distance_along_m) {
            *out_maneuver = m;
            *out_distance_to_m = m.distance_along_route_m - distance_along_m;
            return true;
        }
    }
    return false;
}
