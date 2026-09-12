#pragma once

#include <lvgl.h>
#include <stdint.h>

#include "MapView.h"

// Draws the vector road map behind a MapView's trail.
//
// Shared rather than duplicated: the ROUTE page and the dashboard's inline
// map are both MapViews, and roads belong under both. The first version lived
// inside Page_Map, which is why the dashboard showed a bare trail on a card
// that had roads on it.
//
// One object with a draw callback, not one lv_line per way. A city is
// thousands of ways; thousands of lv_obj would cost memory and a layout pass
// each, where this draws them all in a single pass with no objects at all.
//
// Attaches INSIDE the MapView's container and moves itself behind the trail.
// That placement is load-bearing: MapView paints an opaque background, so a
// sibling created alongside it is drawn and then covered -- silently, with
// every measurement still reporting success.

#ifdef __cplusplus
extern "C" {
#endif

// No-op when no road map is loaded, so callers need not check.
void RoadView_Attach(MapView_t *view);

// Redraw the roads. Call after anything that moves or rescales the MapView.
//
// Needed because the roads are drawn from the view's centre and scale, but
// LVGL only knows to repaint an object when that object changes. Moving the
// view updates the trail's points, which invalidates the trail -- and leaves
// the road layer holding the projection it was last drawn with, so the streets
// stay put while the track slides across them.
void RoadView_Refresh();

// Microseconds spent in the last draw, and segments drawn. This is the number
// that decided vector over raster, so it stays visible rather than being
// measured once and thrown away.
uint32_t RoadView_LastDrawUs();
uint32_t RoadView_LastSegments();

#ifdef __cplusplus
}
#endif
