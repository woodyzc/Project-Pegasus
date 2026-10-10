#pragma once

#include <lvgl.h>

#include "LineRaster.h"

// The surface LineRaster draws into, taken from the draw context of an LVGL
// draw callback: the strip buffer LVGL is rendering, and the clip it gave the
// object being drawn.
//
// Returns false whenever something would apply on the way into that buffer
// that a direct write would skip: a mask (a parent with a rounded clip), a
// display with a set_px_cb, or a layer that carries alpha (LVGL sets
// screen_transp while one is open, and its pixels are three bytes, not two).
// None of those is true of the map on this board. The check is there so the
// day one becomes true, the map falls back to lv_draw_line and gets slower,
// rather than drawing into the wrong pixels.
//
// Per strip, not per frame: LVGL hands each strip its own buffer area.
//
// Always false when built with -D PEGASUS_LINE_RASTER=0.
bool LineRasterLv_Surface(lv_draw_ctx_t *ctx, LineRasterSurface_t *out);
