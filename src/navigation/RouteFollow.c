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

bool RouteFollow_Snap(const uint8_t *blob,
                      const RouteManifest_t *manifest,
                      const uint32_t *cum,
                      double lat,
                      double lon,
                      RouteFix_t *out) {
    if (blob == NULL || manifest == NULL || cum == NULL || out == NULL ||
        manifest->point_count < 2) {
        return false;
    }

    // Work in metres on a plane whose origin is the fix. Over one segment the
    // distortion is immaterial, and it keeps the arithmetic in a range where
    // double has far more precision than the measurement deserves.
    const double lon_scale = LonScale(lat);

    double best_cross = INFINITY;
    uint16_t best_segment = 0;
    double best_along = 0.0;

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

        if (cross < best_cross) {
            best_cross = cross;
            best_segment = i;
            // Interpolate within the segment using the cumulative table, so
            // the answer stays consistent with the length the table reports
            // even where the two formulas would differ slightly.
            const double seg_len = (double)cum[i + 1] - (double)cum[i];
            best_along = (double)cum[i] + t * seg_len;
        }

        a = b;
    }

    if (!isfinite(best_cross)) {
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
