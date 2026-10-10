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

// Draw less while the view is being dragged, and everything again when it
// stops. See the note on ROAD_DRAG_MIN_SEGMENT_PX: the road layer is redrawn
// once per frame throughout a pan, and at ~94us per lv_draw_line that is what
// sets the frame rate.
//
// Clearing it repaints at full detail; setting it does not, because whatever
// set it is about to invalidate anyway.
void RoadView_SetInteractive(bool interactive);

// ---- What drawing the roads costs ----
//
// Every figure describes a BUILD -- one projection of the view -- or the frame
// drawn from it, never a single strip. LVGL draws a frame in 40-line strips
// and calls the layer once per strip; the first version of these figures
// timed one call, which read as a frame and was a seventh of one. See the
// note above RoadDrawCb.

// Segments in the last build, and ways that survived culling for it. Zero
// ways with a loaded map means the roads are somewhere else entirely -- which
// is what a .prd for the wrong town looks like.
uint32_t RoadView_LastSegments();
uint32_t RoadView_LastVisibleWays();

// The build split in two: deciding what is visible, and the whole build
// including that -- decimating and projecting what survived. Split because
// three rounds of optimising the second half once barely moved the total,
// which is the signature of the cost being in the first -- and it was, 9,904us
// of a 12,272us frame. The cull is ~128us via the grid index now, and a
// regression there would otherwise hide inside a total that looks reasonable.
uint32_t RoadView_LastCullUs();
uint32_t RoadView_LastBuildUs();

// lv_draw_line across every strip drawn from the last finished build, and how
// many strips that was. This is the frame's drawing; the build is its
// geometry, and the two add up to what the road layer costs a frame.
uint32_t RoadView_LastFrameDrawUs();
uint16_t RoadView_LastFrameStrips();

// The same, for the last build made WHILE the map was being dragged, held
// until the next drag. Reading the ordinary figures for this is impossible:
// releasing the map repaints at full detail, so the drag numbers are gone
// before anyone can navigate to the page that shows them.
uint32_t RoadView_LastDragBuildUs();
uint32_t RoadView_LastDragFrameDrawUs();
uint16_t RoadView_LastDragSegments();

#ifdef __cplusplus
}
#endif
