#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Anti-aliased round-capped lines, drawn straight into a 16-bit strip.
//
// Pure C and no LVGL: ui/LineRasterLv.h hands it LVGL's draw buffer, and
// test/host/test_line_raster.c checks every pixel it writes against a
// brute-force reference.
//
// ---------------------------------------------------------------------------
// Why the map does not use lv_draw_line
// ---------------------------------------------------------------------------
// The map is lines: every road, the route, the chevrons on it, the rider's
// outline. lv_draw_line is general-purpose, and it pays for that on every
// segment. A skewed line stacks four line masks and runs the generic mask
// machinery over its bounding box row by row, and each round cap is a whole
// lv_draw_rect with a radius mask of its own. The panel measured 50 to 165us
// a segment (RoadView.cpp), with a few hundred segments in a frame. That is
// the road layer's whole cost.
//
// A round-capped line is a capsule: every point within half the width of the
// segment. Across one row a capsule is a single interval, which three cheap
// pieces give exactly: the slab along the segment and the two end discs. The
// pixels well inside the interval are a plain fill. Only the fringe pixel or
// two at each end of the row need a coverage figure, and that is a distance
// that changes linearly along the row, so the inner loop has no square root
// and no division.
//
// ---------------------------------------------------------------------------
// What it draws, exactly
// ---------------------------------------------------------------------------
// A pixel's coverage is R + 0.5 - d, clamped to [0, 1], where d is the
// distance from the pixel's centre to the segment and R is half the width.
// That is a one-pixel ramp across the edge, the usual distance-based
// approximation of area coverage. It is exact for an edge parallel to an axis
// and close for any other.
//
// An even width puts the axis on the pixel boundary, half a pixel along both
// axes, which is where LVGL puts it. So a horizontal or vertical line covers
// exactly `width` rows or columns at full strength, the same pixels
// lv_draw_line fills, and stays as crisp as it was.
//
// A zero-length segment draws nothing, as lv_draw_line does. A polyline's
// joints get their round ends from the segments that meet there.
//
// ⚠️ Not reentrant. It keeps a small cache of per-width tables, and the only
// caller is the LVGL task.

// Widest line it will draw. Anything wider is refused rather than drawn
// thinner, so the caller can hand it to LVGL instead.
#define LINE_RASTER_MAX_WIDTH 12

typedef struct {
    // Row-major RGB565, `stride` pixels per row.
    uint16_t *pixels;
    int32_t stride;
    // The coordinates of pixels[0], in the same space as the points.
    int32_t origin_x;
    int32_t origin_y;
    // The part that may be written, inclusive. Must lie within the buffer;
    // nothing outside it is read or written.
    int32_t clip_x1;
    int32_t clip_y1;
    int32_t clip_x2;
    int32_t clip_y2;
    // Each pixel's two bytes are swapped, as LV_COLOR_16_SWAP stores them.
    bool swapped;
} LineRasterSurface_t;

// Draws a line `width` pixels wide from (ax, ay) to (bx, by), with round ends.
// `colour` is in the surface's own byte order.
//
// Returns false, having drawn nothing, for a width it does not take
// (below 1 or above LINE_RASTER_MAX_WIDTH). True otherwise, including for a
// line that misses the clip area entirely.
bool LineRaster_Line(const LineRasterSurface_t *surface, int32_t ax, int32_t ay, int32_t bx,
                     int32_t by, int32_t width, uint16_t colour);

// Fills pixels x1..x2 of row y, clipped. No anti-aliasing: for shapes that
// were stacks of one-row rects and should stay exactly that.
void LineRaster_Span(const LineRasterSurface_t *surface, int32_t y, int32_t x1, int32_t x2,
                     uint16_t colour);

// `src` over `dst` at `alpha` out of 255, with the rounding lv_color_mix
// uses, so a fringe pixel comes out as LVGL would have blended it. Both are
// in the byte order `swapped` says.
uint16_t LineRaster_Blend(uint16_t dst, uint16_t src, uint8_t alpha, bool swapped);

#ifdef __cplusplus
}
#endif
