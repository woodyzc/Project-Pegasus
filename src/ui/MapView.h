#pragma once

#include <lvgl.h>

#include "../navigation/MapProject.h"
#include "../system/DataCenter.h"

// A breadcrumb map that can be dropped into any parent at any size.
//
// It exists because the map now appears twice: inline on the dashboard, as the
// navigation slot when the rider has chosen GPX, and full screen on Page_Map.
// Those are the same picture at two sizes, so they are one widget rather than
// two copies of the projection and redraw code.
//
// Point storage is caller-owned: two live views would otherwise each want
// their own arrays, and making that allocation explicit at the call site keeps
// the cost visible rather than hidden inside a constructor.

typedef struct {
    lv_obj_t *container;
    lv_obj_t *trail;
    lv_obj_t *marker;

    lv_point_t *points;     // caller-owned, `capacity` entries
    MapPoint_t *projected;  // caller-owned, `capacity` entries
    size_t capacity;

    lv_point_t marker_points[4]; // heading triangle, last point closes it

    lv_coord_t width;
    lv_coord_t height;

    double metres_per_pixel;
    double center_lat;
    double center_lon;
    bool have_center;

    // True once the rider has chosen a scale, which stops FitTrack and
    // SetPosition from overriding it.
    bool zoom_locked;

    // Set once the rider drags. Stops SetPosition recentring on the fix.
    bool pan_locked;
} MapView_t;

// Builds the view inside `parent`. `points` and `projected` must each hold
// `capacity` entries and outlive the view -- lv_line keeps a pointer to
// `points` rather than copying it.
void MapView_Create(MapView_t *view, lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                    lv_coord_t h, lv_point_t *points, MapPoint_t *projected, size_t capacity);

// Frames the whole loaded trail. Used before a fix arrives, so the first look
// shows the route rather than an arbitrary zoom.
void MapView_FitTrack(MapView_t *view);

// Centres on the rider and points the marker along `heading`. Call on each
// position update; a fix that is not valid hides the marker instead.
void MapView_SetPosition(MapView_t *view, const GPS_Info_t *gps);

// Reprojects and redraws at the current centre and scale.
void MapView_Redraw(MapView_t *view);

// Metres covered by the view's full width, for a scale readout.
double MapView_MetresAcross(const MapView_t *view);

// ---- Zoom ----
// Steps through a fixed ladder of scales rather than scaling by an arbitrary
// factor, so repeated presses always land on the same set of views and the
// road detail thresholds (RoadView) line up with recognisable steps.
//
// A zoom set by hand sticks: MapView_SetPosition keeps following the rider but
// stops re-fitting the scale, because a view that silently re-zooms under a
// rider who just chose one is worse than no zoom at all.
void MapView_ZoomIn(MapView_t *view);
void MapView_ZoomOut(MapView_t *view);
bool MapView_CanZoomIn(const MapView_t *view);
bool MapView_CanZoomOut(const MapView_t *view);

// ---- Panning ----
// Shifts the view by a screen delta. Dragging the map right moves the view
// west, the way dragging paper across a desk does; the alternative reads as
// moving a window over a fixed world and nobody expects that on a touchscreen.
//
// Panning locks the centre for the same reason zooming locks the scale: a view
// that snaps back to the rider mid-drag is unusable. MapView_Recenter undoes
// both and resumes following.
void MapView_PanPixels(MapView_t *view, lv_coord_t dx, lv_coord_t dy);

// Back to following the rider, at an automatic scale. Clears both locks.
void MapView_Recenter(MapView_t *view);

// True when the view has been moved or zoomed by hand, so the UI can offer a
// way back rather than leaving someone stranded over empty countryside.
bool MapView_IsManual(const MapView_t *view);
