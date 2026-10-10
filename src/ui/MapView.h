#pragma once

#include <lvgl.h>

#include "../navigation/MapHeading.h"
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

// Chevrons a view will lay out along the route still to ride. At 34px apart
// that is over 3,000px of drawn route -- the whole visible track zoomed out on
// a winding route, with room to spare. Past it the far end goes unmarked
// rather than anything overflowing.
#define MAP_MAX_CHEVRONS 96

typedef struct {
    lv_obj_t *container;
    // The trail in two pieces: what has been ridden and what has not. Two
    // lv_line objects rather than one, because a line has a single colour and
    // the whole point is that the two halves differ.
    lv_obj_t *trail_done;
    lv_obj_t *trail;
    lv_obj_t *marker;

    lv_point_t *points;     // caller-owned, `capacity` entries
    MapPoint_t *projected;  // caller-owned, `capacity` entries
    size_t capacity;

    lv_point_t marker_points[4]; // heading triangle, last point closes it

    // Whether the rider's triangle is drawn. A flag rather than hiding the
    // object, because that object also carries the chevrons, the north arrow
    // and the scale bar: hiding it on a lost fix took all three with it, so
    // before the first fix -- and for every stale spell after it -- the route
    // had no direction marks and the scale label sat over a bar that was not
    // there.
    bool show_rider;

    // Where the rider is, which is NOT where the view is centred once the
    // rider has dragged the map. Redraw places the triangle by projecting
    // this; it used to be drawn at the centre of the view unconditionally, so
    // after a drag it marked the place being looked at rather than the place
    // the rider was.
    double rider_lat;
    double rider_lon;
    float rider_heading_deg;

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

    // ⚠️ Whether this view's redraws may write the shared camera.
    //
    // There are two MapViews -- the dashboard's tile and the ROUTE page's
    // full screen -- and one saved camera between them. MapView_Redraw saves
    // on every pass, which is deliberate (see the note at the save), but the
    // dashboard's refresh timer is created in onViewLoad and the page is
    // CACHED, so that timer keeps running while the route page is on top.
    // Every fix therefore redrew the hidden dashboard tile and overwrote the
    // camera the rider was actively zooming.
    //
    // The symptom was zoom "sometimes" reverting on leaving the map: it
    // depended on whether a fix landed between the rider's last zoom and
    // their exit.
    bool camera_owner;

    // What the last Redraw actually put on screen, so the overlay callback can
    // decorate the same geometry rather than recomputing it. Chevrons go on
    // the stretch still to ride, which is [ahead_from, count).
    size_t drawn_count;
    size_t drawn_ahead_from;

    // The chevrons themselves, laid out by Redraw: apex then the two arm ends,
    // in container coordinates. Computed there rather than in the overlay's
    // draw callback because that callback runs once per 40-line strip LVGL
    // draws a frame in -- up to seven times -- and walking the polyline in
    // software double for every strip was the same answer worked out seven
    // times over.
    lv_point_t chevrons[MAP_MAX_CHEVRONS][3];
    uint16_t chevron_count;

    // Scale bar, chosen by MapScale_Choose each Redraw. Zero hides it, which
    // is what an unusable scale should do rather than drawing a bar that lies.
    int scale_px;
    lv_obj_t *scale_label;
    // What that label already says. lv_label_set_text invalidates whether or
    // not the text differs (CLAUDE.md 8a), and the scale bar is rewritten on
    // every redraw -- so this is what stops a pan, which cannot change the
    // scale at all, from re-rasterising the figure on every frame of itself.
    char scale_text[16];

    // The ridden/ahead split is no longer state of the view. It is read from
    // navigation/GpxProgress.h on every redraw, and the reasons it moved out
    // are both about this struct's lifetime: the ROUTE page's view is rebuilt
    // on every visit, and a view hears fixes only while its page is on
    // screen. See that header.
    //
    // What it hands back is a SOURCE index, and has to be: drawn indices are
    // not stable between frames -- the builder clips to the viewport and thins
    // what it draws, so the same drawn index means a different place from one
    // redraw to the next. Redraw finds the drawn vertex nearest that source
    // point instead.

    // Track-up. The smoother decides what "up" is and when it has moved
    // enough to be worth a redraw; this view only asks it.
    MapHeading_t heading;
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
//
// Returns whether the view moved, which is what decides whether the road layer
// needs redrawing. A stopped bike's fix is mostly receiver wander, and
// following it redrew the whole map every second to move it a few pixels; see
// MAP_STILL_DEADBAND_M.
bool MapView_SetPosition(MapView_t *view, const GPS_Info_t *gps);

// Reprojects and redraws at the current centre, scale and heading.
void MapView_Redraw(MapView_t *view);

// The angle the view is currently drawn at, and whether it is turned at all.
// Zero and false mean north-up, which is what a device that has never moved
// honestly knows.
bool MapView_IsTrackUp(const MapView_t *view);
double MapView_HeadingDeg(const MapView_t *view);

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

// ---- The camera the two map views share ----
//
// The dashboard's inline map and the full-screen route page are separate
// MapView_t instances of different sizes, and each used to frame the track
// itself on load. So enlarging the map, panning it, going back and enlarging
// it again threw the rider's framing away and refitted -- which reads as the
// map resetting itself for no reason.
//
// Saving where the map is looking rather than which page was looking at it
// keeps the two in step. The scale travels as metres per pixel, which means
// the same thing in a 184px tile and a 262px page; the taller one simply shows
// more of the same ground.
// Only the view the rider is actually looking at may write the shared camera
// from its redraws. Set on appear, cleared on disappear; explicit
// MapView_SaveCamera calls are unaffected.
void MapView_SetCameraOwner(MapView_t *view, bool owner);

void MapView_SaveCamera(const MapView_t *view);

// Applies the saved camera. False when nothing has been saved yet, which is
// the caller's cue to frame the track instead.
bool MapView_RestoreCamera(MapView_t *view);

// Forgets it, so the next page to open frames the track again. For loading a
// different route, where the old camera points at another part of the world.
void MapView_ForgetCamera();
// ---- What the last redraw cost (CLAUDE.md 8a) ----
//
// On the panel rather than over serial, for the reason 8 gives: there is no
// usable serial console on this board, and "the map feels slow with this
// route loaded" cannot be answered by reasoning about it. Guessing at
// performance here has cost four rounds before now; a figure costs a
// micros() call.
//
// Two of them, because only one can be read live. The first is the map
// standing still; the second is the last build taken WHILE the finger was
// down, held since -- which is the number that decides whether panning feels
// smooth, and which cannot be watched as it happens because releasing the map
// repaints before anyone can reach the settings page.
void MapView_SetInteractive(bool interactive);
uint32_t MapView_LastBuildUs();
uint32_t MapView_LastBuildDragUs();

// Source points walked, against drawn points produced. The gap between them
// is the whole reason the visible-range search is written the way it is: a
// 20,000-point route draws at most a couple of hundred.
uint32_t MapView_LastSourcePoints();
uint32_t MapView_LastDrawnPoints();


