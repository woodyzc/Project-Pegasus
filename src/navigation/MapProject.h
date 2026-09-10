#pragma once

#include <stdint.h>
#include <stddef.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#include "TrackBuffer.h"

// Turns a breadcrumb trail into screen coordinates (CLAUDE.md §5).
//
// Equirectangular projection, centred on the rider: x scales by
// cos(latitude) so a degree of longitude shrinks correctly away from the
// equator, and y is negated because screen y grows downward. That projection
// distorts badly at continental scale but is exact enough over the few
// kilometres a 240x320 panel can usefully show, and costs one cosine per
// redraw instead of the transcendentals a Mercator would.
//
// Pure -- no LVGL, no Arduino -- so the geometry is covered by test/host and
// Page_Map only has to draw the points it is handed.

#define MAP_EARTH_METRES_PER_DEGREE 111320.0

// Screen coordinates are clamped to this magnitude. A trail point can be
// hundreds of kilometres from the viewport, which would overflow int16 and
// wrap a line back across the screen; clamping keeps the segment pointing the
// right way off-edge instead.
#define MAP_COORD_LIMIT 4096

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t x;
    int16_t y;
} MapPoint_t;

// Projects one coordinate relative to the view centre.
void Map_Project(double lat, double lon, double center_lat, double center_lon,
                 double metres_per_pixel, int16_t center_x, int16_t center_y,
                 int16_t *out_x, int16_t *out_y);

// Metres per pixel that fits a bounding box into width x height, leaving
// `margin_px` on every side. Never returns zero or a negative: a track with no
// extent (one point, or a rider standing still) still needs a usable scale.
double Map_FitScale(double min_lat, double max_lat, double min_lon, double max_lon,
                    int16_t width, int16_t height, int16_t margin_px);

// Projects a whole track into a polyline for drawing.
//
// Consecutive points landing on the same pixel are collapsed -- a GPS logging
// at 1Hz produces hundreds of points per pixel when a rider is stopped at a
// light, and drawing them all costs time and changes nothing on screen.
// Returns the number of points written, at most `max_points`.
size_t Map_BuildPolyline(const TrackBuffer_t *track, double center_lat, double center_lon,
                         double metres_per_pixel, int16_t center_x, int16_t center_y,
                         MapPoint_t *out, size_t max_points);

#ifdef __cplusplus
}
#endif
