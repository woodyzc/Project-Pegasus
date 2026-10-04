#include "MapView.h"

#include <math.h>
#include <string.h>

#include "../navigation/MapScale.h"

// How far off the line a fix may be and still count as progress along it.
// Generous: a 240px panel at a usable zoom is wider than this, and the point
// is to reject a fix from somewhere else entirely rather than to police
// accuracy.
#ifndef MAP_DONE_MAX_OFFTRACK_M
#define MAP_DONE_MAX_OFFTRACK_M 200.0
#endif
#include <stdint.h>

#include "../navigation/GpxTrack.h"
#include "../system/Settings.h"

namespace {

constexpr uint32_t COLOR_MAP_BG = 0x0B1116;
// The trail, in two colours: what is behind the rider and what is ahead.
//
// Magenta ahead, grey behind. The magenta is the convention every other head
// unit uses for a planned route -- Garmin and Wahoo both -- and conventions
// are worth adopting for exactly the reason §8 gives for the red REC dot: a
// panel read in a fifth of a second is better served by a learned cue than by
// a reasoned one.
//
// It replaces green-ahead/amber-behind, which was reasoned and fine, but
// which spent two saturated colours on one object. Grey behind says "done"
// without competing, and frees amber for things that actually want attention.
constexpr uint32_t COLOR_TRAIL_DONE = 0x6B7680;
constexpr uint32_t COLOR_TRAIL_AHEAD = 0xF02D9B;

// Direction chevrons, laid along the stretch still to ride. White, because
// they sit ON the magenta and have to read at 7px.
constexpr uint32_t COLOR_CHEVRON = 0xFFFFFF;

// Pixels between chevrons along the drawn line. Close enough that a junction
// always has one near it, far enough that the route still reads as a line
// rather than a dotted one.
constexpr double CHEVRON_SPACING_PX = 34.0;
constexpr double CHEVRON_LEN_PX = 7.0;
constexpr double CHEVRON_HALF_W_PX = 4.5;

// The scale bar and the north arrow live in opposite corners, out of the way
// of the rider marker which sits at the centre when the map is following.
constexpr uint32_t COLOR_HUD = 0xC8D2DC;
constexpr lv_coord_t HUD_MARGIN_PX = 6;
constexpr lv_coord_t SCALE_MAX_PX = 74;
constexpr lv_coord_t NORTH_RADIUS_PX = 9;

constexpr uint32_t COLOR_MARKER = 0x61DAFB;

// Outlined in the map's own background colour, so the marker keeps a hard
// edge wherever it sits. It is drawn ON TOP of the route and the roads, and
// every one of those is light -- white-grey streets, magenta route, white
// chevrons -- so a cyan triangle laid directly on them loses its shape at the
// moment it matters, which is when the rider is on the line they are
// following. Dark separates it from all of them at once. Over bare map this
// outline is invisible, which is the correct behaviour rather than a flaw.
constexpr uint32_t COLOR_MARKER_EDGE = COLOR_MAP_BG;

// Half-height of the heading triangle: ~22px tall and ~15px wide.
//
// 14, doubled from 7. At 7 it was 11px tall and read as a speck once the
// roads around it were widened for legibility -- the thing that says WHERE
// THE RIDER IS was the faintest mark on its own map. Every head unit worth
// copying draws this big; the reference that prompted the change gives it
// roughly a tenth of the map's height, which is what this is.
//
// It is still small enough not to hide the trail: the triangle covers about
// 165 square px of a 240x184 tile, and it is the one thing allowed to sit on
// top of the route because it is the rider's own position.
constexpr double MARKER_RADIUS = 14.0;

// The triangle is FILLED, and that needs a draw callback rather than a widget.
//
// It used to be an lv_line tracing the three corners, which looked solid only
// because it was 11px tall and stroked 3px -- the stroke very nearly met in
// the middle. Doubling the size turns that same object into a wireframe
// outline, so "make it bigger" could not be done by changing one number.
// LVGL 8 has no filled-polygon primitive outside lv_canvas, and a canvas
// would mean a per-view pixel buffer redrawn on every fix for a shape that is
// three points; scanline-filling it here costs ~22 rect draws at 1Hz and no
// memory at all. RoadView draws its whole road network this way for the same
// reason.
// Fills a triangle by scanlines, in container-relative coordinates.
//
// LVGL 8 has no filled-polygon primitive outside lv_canvas, and a canvas would
// mean a per-view pixel buffer redrawn on every fix. Rects rather than 1px
// lines: a horizontal line is anti-aliased at both ends, so a stack of them
// builds a shape with soft ragged sides, where a rect of one row is exact.
//
// Extracted from the marker when the chevrons and the north arrow needed the
// same thing. RoadView draws its whole network this way for the same reason.
void FillTriangle(lv_draw_ctx_t *ctx, const lv_area_t &area, const lv_point_t p[3],
                  uint32_t colour) {
    lv_coord_t min_y = p[0].y;
    lv_coord_t max_y = p[0].y;
    for (int i = 1; i < 3; i++) {
        if (p[i].y < min_y) min_y = p[i].y;
        if (p[i].y > max_y) max_y = p[i].y;
    }

    lv_draw_rect_dsc_t fill;
    lv_draw_rect_dsc_init(&fill);
    fill.bg_color = lv_color_hex(colour);
    fill.bg_opa = LV_OPA_COVER;
    fill.border_width = 0;
    fill.radius = 0;

    for (lv_coord_t y = min_y; y <= max_y; y++) {
        double xs[3];
        int n = 0;
        for (int i = 0; i < 3; i++) {
            const lv_point_t a = p[i];
            const lv_point_t b = p[(i + 1) % 3];
            if (a.y == b.y) {
                continue; // horizontal edge: the other two already span it
            }
            const lv_coord_t lo_y = a.y < b.y ? a.y : b.y;
            const lv_coord_t hi_y = a.y < b.y ? b.y : a.y;
            if (y < lo_y || y > hi_y) {
                continue;
            }
            xs[n++] = (double)a.x + (double)(b.x - a.x) * (double)(y - a.y) / (double)(b.y - a.y);
        }
        if (n < 2) {
            continue;
        }
        double lo = xs[0];
        double hi = xs[0];
        for (int i = 1; i < n; i++) {
            if (xs[i] < lo) lo = xs[i];
            if (xs[i] > hi) hi = xs[i];
        }
        lv_area_t row;
        row.x1 = (lv_coord_t)(area.x1 + lo);
        row.x2 = (lv_coord_t)(area.x1 + hi);
        row.y1 = (lv_coord_t)(area.y1 + y);
        row.y2 = row.y1;
        if (row.x2 < row.x1) {
            continue;
        }
        lv_draw_rect(ctx, &fill, &row);
    }
}


// Chevrons along the stretch still to ride.
//
// The one thing a plain line cannot say is WHICH WAY along itself. On an
// out-and-back the route crosses its own path, and at a junction the question
// "do I go left here or is that the leg I come back on" is exactly the one a
// head unit exists to answer.
//
// Walked in pixels on the drawn polyline rather than in metres on the source,
// so spacing stays even on screen at any zoom -- which is the only place it
// is read.
void DrawChevrons(lv_draw_ctx_t *ctx, const lv_area_t &area, const MapView_t *view) {
    if (view->drawn_count < 2 || view->drawn_ahead_from + 1 >= view->drawn_count) {
        return;
    }

    double carry = CHEVRON_SPACING_PX * 0.5; // first one half a gap in
    for (size_t i = view->drawn_ahead_from; i + 1 < view->drawn_count; i++) {
        const double ax = view->points[i].x;
        const double ay = view->points[i].y;
        const double bx = view->points[i + 1].x;
        const double by = view->points[i + 1].y;
        const double dx = bx - ax;
        const double dy = by - ay;
        const double len = sqrt((dx * dx) + (dy * dy));
        if (len < 0.5) {
            continue; // a thinned polyline can repeat a point
        }
        const double ux = dx / len;
        const double uy = dy / len;

        for (double at = carry; at < len; at += CHEVRON_SPACING_PX) {
            const double cx = ax + (ux * at);
            const double cy = ay + (uy * at);

            // Tip forward along the segment, base either side of it.
            lv_point_t tri[3];
            tri[0].x = (lv_coord_t)(cx + (ux * CHEVRON_LEN_PX));
            tri[0].y = (lv_coord_t)(cy + (uy * CHEVRON_LEN_PX));
            tri[1].x = (lv_coord_t)(cx - (uy * CHEVRON_HALF_W_PX));
            tri[1].y = (lv_coord_t)(cy + (ux * CHEVRON_HALF_W_PX));
            tri[2].x = (lv_coord_t)(cx + (uy * CHEVRON_HALF_W_PX));
            tri[2].y = (lv_coord_t)(cy - (ux * CHEVRON_HALF_W_PX));
            FillTriangle(ctx, area, tri, COLOR_CHEVRON);
        }
        // Carry the remainder into the next segment, so spacing does not
        // restart at every vertex -- on a thinned line that would cluster them
        // wherever the geometry happens to bend.
        carry = fmod(carry - len, CHEVRON_SPACING_PX);
        if (carry < 0.0) {
            carry += CHEVRON_SPACING_PX;
        }
    }
}

// Which way is north, for a map that has turned.
//
// Pointless north-up, and drawn anyway rather than hidden: a corner that
// sometimes holds a thing and sometimes does not is harder to read than one
// that always does, and the arrow pointing straight up IS the information
// when the map is north-up.
void DrawNorth(lv_draw_ctx_t *ctx, const lv_area_t &area, const MapView_t *view) {
    const double heading = MapView_IsTrackUp(view) ? MapView_HeadingDeg(view) : 0.0;
    // Screen-up is the heading, so north sits at minus the heading. Screen y
    // grows downward, which the sine term accounts for.
    const double a = -heading * M_PI / 180.0;
    const double cx = (double)(view->width - HUD_MARGIN_PX - NORTH_RADIUS_PX);
    const double cy = (double)(HUD_MARGIN_PX + NORTH_RADIUS_PX);

    const double ux = sin(a);
    const double uy = -cos(a);
    const double r = (double)NORTH_RADIUS_PX;

    lv_point_t tri[3];
    tri[0].x = (lv_coord_t)(cx + (ux * r));
    tri[0].y = (lv_coord_t)(cy + (uy * r));
    tri[1].x = (lv_coord_t)(cx - (ux * r * 0.45) - (uy * r * 0.5));
    tri[1].y = (lv_coord_t)(cy - (uy * r * 0.45) + (ux * r * 0.5));
    tri[2].x = (lv_coord_t)(cx - (ux * r * 0.45) + (uy * r * 0.5));
    tri[2].y = (lv_coord_t)(cy - (uy * r * 0.45) - (ux * r * 0.5));
    FillTriangle(ctx, area, tri, COLOR_HUD);
}

// The bar itself. Its label is an ordinary lv_label, because drawing text
// through a draw context means carrying a font descriptor around for no gain.
void DrawScaleBar(lv_draw_ctx_t *ctx, const lv_area_t &area, const MapView_t *view) {
    if (view->scale_px <= 0) {
        return;
    }
    const lv_coord_t y = (lv_coord_t)(view->height - HUD_MARGIN_PX - 2);
    const lv_coord_t x2 = (lv_coord_t)(view->width - HUD_MARGIN_PX);
    const lv_coord_t x1 = (lv_coord_t)(x2 - view->scale_px);

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = lv_color_hex(COLOR_HUD);
    dsc.width = 2;

    lv_point_t a = {(lv_coord_t)(area.x1 + x1), (lv_coord_t)(area.y1 + y)};
    lv_point_t b = {(lv_coord_t)(area.x1 + x2), (lv_coord_t)(area.y1 + y)};
    lv_draw_line(ctx, &dsc, &a, &b);

    // End ticks, so the bar reads as a measured span rather than a stray line.
    for (int i = 0; i < 2; i++) {
        const lv_coord_t x = (i == 0) ? x1 : x2;
        lv_point_t t1 = {(lv_coord_t)(area.x1 + x), (lv_coord_t)(area.y1 + y - 4)};
        lv_point_t t2 = {(lv_coord_t)(area.x1 + x), (lv_coord_t)(area.y1 + y)};
        lv_draw_line(ctx, &dsc, &t1, &t2);
    }
}

void MarkerDrawCb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    MapView_t *view = (MapView_t *)lv_obj_get_user_data(obj);
    if (view == nullptr) {
        return;
    }
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);

    lv_area_t area;
    lv_obj_get_coords(obj, &area);

    // Order matters: chevrons sit on the route and under the rider, the HUD
    // sits over both.
    DrawChevrons(ctx, area, view);

    // marker_points[0..2] are the corners; [3] repeats [0] to close the
    // outline stroke below.
    const lv_point_t *p = view->marker_points;

    FillTriangle(ctx, area, p, COLOR_MARKER);

    // The border last, over the fill, so it trims the stepped edge the
    // scanlines leave and separates the marker from whatever it is sitting on.
    //
    // Drawn around a triangle pushed OUT from the centroid, not around the
    // filled one. A stroke is centred on its path, so an outline traced on the
    // fill's own edge spends half its width eating the fill -- and because
    // this border is the background colour, that does not read as a border
    // over bare map, it just makes the marker smaller. Measured: it cost 4px
    // of width and 6px of height, giving back most of the enlargement this
    // change exists to deliver. Pushing the path outward puts the whole stroke
    // outside the silhouette, so the cyan keeps its full size and the border
    // is added to it rather than subtracted from it.
    //
    // 1.15 leaves the stroke's inner edge a fraction inside the fill, which is
    // deliberate: exactly abutting would let the background show through the
    // seam on some rotations.
    const double EDGE_PUSH = 1.15;
    const double gx = (p[0].x + p[1].x + p[2].x) / 3.0;
    const double gy = (p[0].y + p[1].y + p[2].y) / 3.0;
    lv_point_t out[4];
    for (int i = 0; i < 3; i++) {
        out[i].x = (lv_coord_t)(gx + (p[i].x - gx) * EDGE_PUSH);
        out[i].y = (lv_coord_t)(gy + (p[i].y - gy) * EDGE_PUSH);
    }
    out[3] = out[0];

    lv_draw_line_dsc_t edge;
    lv_draw_line_dsc_init(&edge);
    edge.color = lv_color_hex(COLOR_MARKER_EDGE);
    edge.width = 2;
    edge.round_start = 1;
    edge.round_end = 1;
    for (int i = 0; i < 3; i++) {
        lv_point_t a = {(lv_coord_t)(area.x1 + out[i].x), (lv_coord_t)(area.y1 + out[i].y)};
        lv_point_t b = {(lv_coord_t)(area.x1 + out[i + 1].x),
                        (lv_coord_t)(area.y1 + out[i + 1].y)};
        lv_draw_line(ctx, &edge, &a, &b);
    }

    DrawNorth(ctx, area, view);
    DrawScaleBar(ctx, area, view);
}

} // namespace

void MapView_Create(MapView_t *view, lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                    lv_coord_t h, lv_point_t *points, MapPoint_t *projected, size_t capacity) {
    MapHeading_Reset(&view->heading);
    if (view == nullptr) {
        return;
    }

    view->points = points;
    view->projected = projected;
    view->capacity = capacity;
    view->width = w;
    view->height = h;
    view->metres_per_pixel = 5.0;
    view->center_lat = 0.0;
    view->center_lon = 0.0;
    view->have_center = false;
    view->zoom_locked = false;
    view->pan_locked = false;
    // Explicit, because MapView_t is not required to arrive zeroed and a
    // high-water mark inherited from whatever was in memory would colour an
    // arbitrary stretch of a fresh route as already ridden.
    view->done_src = 0;
    view->have_done_src = false;
    view->done_src_track_points = 0;
    view->done_src_track_name[0] = '\0';
    // Claimed on appear, never by default: a view nobody is looking at must
    // not own the camera.
    view->camera_owner = false;
    view->drawn_count = 0;
    view->drawn_ahead_from = 0;
    view->scale_px = 0;
    view->scale_label = nullptr;

    view->container = lv_obj_create(parent);
    // Not scrollable, and this is what made swipes on the dashboard fail.
    //
    // lv_obj_create() sets LV_OBJ_FLAG_SCROLLABLE by default -- the same
    // default that made this layer's sibling swallow every tap until
    // CLICKABLE was cleared (see RoadView_Attach). The consequence here is
    // subtler: LVGL's indev_gesture() begins with
    //
    //     if (proc->types.pointer.scroll_obj) return;
    //
    // so once a drag has latched onto any scrollable object, gesture
    // detection does not run at all. In GPX mode this container IS the top
    // 184px of the dashboard, so a swipe that started on the map was decided
    // to be a scroll and no page change ever followed. Swipes that began on
    // the metric cells worked, because those clear the flag -- which is
    // exactly what "stiff, works sometimes" felt like.
    //
    // Panning is unaffected: it runs off LV_EVENT_PRESSING and
    // lv_indev_get_vect (Page_Map's OnMapPressing), never off LVGL scrolling.
    lv_obj_clear_flag(view->container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(view->container, w, h);
    lv_obj_set_pos(view->container, x, y);
    lv_obj_set_style_bg_color(view->container, lv_color_hex(COLOR_MAP_BG), 0);
    // Borderless and square, because both callers give it the full width of
    // the panel: a frame here would draw a line right at the screen edge,
    // which is the one place the layout deliberately has none. The hairlines
    // that divide it from its neighbours are the caller's to place.
    lv_obj_set_style_border_width(view->container, 0, 0);
    lv_obj_set_style_radius(view->container, 0, 0);
    lv_obj_set_style_pad_all(view->container, 0, 0);
    // Clip rather than scroll: a trail point can project far outside the
    // viewport, and LVGL would otherwise grow a scrollable area around it and
    // let a stray touch drag the map into empty space.
    lv_obj_clear_flag(view->container, LV_OBJ_FLAG_SCROLLABLE);

    // Created first, so it sits behind the ridden half: where the two overlap
    // by the one shared point, the ridden colour wins and the join is clean.
    view->trail = lv_line_create(view->container);
    lv_obj_set_style_line_color(view->trail, lv_color_hex(COLOR_TRAIL_AHEAD), 0);
    // 6px, and it tracks the roads rather than being chosen on its own. The
    // roads under it are drawn 2 to 5px wide, and a route the same weight as
    // the streets it crosses is a route the eye has to hunt for. This is the
    // one line on the map that is not scenery.
    //
    // It was 4 while the roads were 1 to 4. Widening them for legibility
    // without widening this would have quietly handed the top of the
    // hierarchy to the rivers -- the fix for one problem creating another,
    // one file away, with nothing to catch it but looking at the screen.
    lv_obj_set_style_line_width(view->trail, 6, 0);
    lv_obj_set_style_line_rounded(view->trail, true, 0);
    lv_obj_set_pos(view->trail, 0, 0);

    view->trail_done = lv_line_create(view->container);
    lv_obj_set_style_line_color(view->trail_done, lv_color_hex(COLOR_TRAIL_DONE), 0);
    lv_obj_set_style_line_width(view->trail_done, 6, 0);
    lv_obj_set_style_line_rounded(view->trail_done, true, 0);
    lv_obj_set_pos(view->trail_done, 0, 0);

    // A triangle rather than a dot: the rider's heading is already published
    // and carries real information at a junction -- which way am I pointing
    // relative to the line I am supposed to be following.
    view->marker = lv_obj_create(view->container);
    lv_obj_clear_flag(view->marker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(view->marker, 0, 0);
    lv_obj_set_size(view->marker, w, h);
    lv_obj_set_style_bg_opa(view->marker, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(view->marker, 0, 0);
    lv_obj_set_style_pad_all(view->marker, 0, 0);
    lv_obj_clear_flag(view->marker, LV_OBJ_FLAG_SCROLLABLE);
    // Not clickable. lv_obj_create sets LV_OBJ_FLAG_CLICKABLE by default and
    // this object covers the whole tile, so leaving it set would swallow every
    // touch on the map -- the dashboard's navigation tile would stop opening
    // the ROUTE page, and its zoom buttons would stop answering. The lv_line
    // this replaces cleared the flag in its own constructor, so the hazard
    // arrives with the switch to lv_obj rather than being visible in the diff.
    lv_obj_clear_flag(view->marker, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(view->marker, view);
    lv_obj_add_event_cb(view->marker, MarkerDrawCb, LV_EVENT_DRAW_MAIN, nullptr);

    // Created after the overlay so it draws on top of the bar it labels.
    view->scale_label = lv_label_create(view->container);
    lv_label_set_text(view->scale_label, "");
    lv_obj_set_style_text_font(view->scale_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(view->scale_label, lv_color_hex(COLOR_HUD), 0);
    lv_obj_clear_flag(view->scale_label, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(view->marker, LV_OBJ_FLAG_HIDDEN);
}

void MapView_FitTrack(MapView_t *view) {
    if (view == nullptr || GpxTrack_PointCount() == 0) {
        return;
    }

    double min_lat;
    double max_lat;
    double min_lon;
    double max_lon;
    if (!GpxTrack_Bounds(&min_lat, &max_lat, &min_lon, &max_lon)) {
        return;
    }
    if (!GpxTrack_Center(&view->center_lat, &view->center_lon)) {
        return;
    }

    view->have_center = true;
    // A scale the rider chose outranks fitting the track. Re-fitting under
    // someone who just pressed zoom is the surest way to make the control feel
    // broken.
    if (!view->zoom_locked) {
        view->metres_per_pixel =
            Map_FitScale(min_lat, max_lat, min_lon, max_lon, view->width, view->height, 10);
    }
    MapView_Redraw(view);
}

void MapView_SetPosition(MapView_t *view, const GPS_Info_t *gps) {
    if (view == nullptr || gps == nullptr) {
        return;
    }

    if (!gps->fix_valid) {
        lv_obj_add_flag(view->marker, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // Just recorded. Working out where this falls on the line is Redraw's
    // job, because only Redraw knows which stretch of the track is actually
    // being drawn -- and that changes with every pan and zoom.
    view->fix_lat = gps->lat;
    view->fix_lon = gps->lon;
    view->have_fix = true;

    // Once there is a fix the view follows the rider: what matters while
    // riding is where you are on the line, not the shape of the whole route.
    //
    // Unless the rider has dragged the map, in which case they are looking at
    // somewhere specific and yanking the view back to the bike every second
    // makes the gesture pointless. The marker below still updates, so the
    // rider's position stays visible while they look ahead.
    if (!view->pan_locked) {
        view->center_lat = gps->lat;
        view->center_lon = gps->lon;
        view->have_center = true;
    }

    // Offered every fix. The smoother is what decides whether this one is
    // usable at all -- a stationary receiver reports the direction of its own
    // noise, and rotating a map on that is worse than not rotating it.
    MapHeading_Feed(&view->heading, gps->fix_valid, gps->speed, gps->heading);

    {
        // With the map turned, the rider's triangle is fixed pointing up: the
        // view is doing the turning now, and a marker that also turned would
        // turn twice. North-up keeps the old behaviour, where the triangle is
        // the only thing that says which way the rider faces.
        const bool track_up = Settings_GetMapTrackUp() && MapHeading_Valid(&view->heading);
        const double drawn_deg =
            track_up ? (gps->heading - MapHeading_Degrees(&view->heading)) : gps->heading;
        const double rad = drawn_deg * M_PI / 180.0;
        const double cx = view->width / 2.0;
        const double cy = view->height / 2.0;
        const double back = 2.4; // radians offset to the two trailing corners

        view->marker_points[0].x = (lv_coord_t)(cx + MARKER_RADIUS * sin(rad));
        view->marker_points[0].y = (lv_coord_t)(cy - MARKER_RADIUS * cos(rad));
        view->marker_points[1].x = (lv_coord_t)(cx + MARKER_RADIUS * 0.8 * sin(rad + back));
        view->marker_points[1].y = (lv_coord_t)(cy - MARKER_RADIUS * 0.8 * cos(rad + back));
        view->marker_points[2].x = (lv_coord_t)(cx + MARKER_RADIUS * 0.8 * sin(rad - back));
        view->marker_points[2].y = (lv_coord_t)(cy - MARKER_RADIUS * 0.8 * cos(rad - back));
        view->marker_points[3] = view->marker_points[0]; // close the triangle

        // The points live in the struct and the callback reads them, so the
        // object has to be told its content changed -- there is no
        // lv_line_set_points to do it here any more.
        lv_obj_invalidate(view->marker);
        lv_obj_clear_flag(view->marker, LV_OBJ_FLAG_HIDDEN);
    }

    MapView_Redraw(view);
}

void MapView_Redraw(MapView_t *view) {
    if (view == nullptr || view->trail == nullptr) {
        return;
    }

    // First, and before any early return below.
    //
    // This used to sit at the bottom, on the reasoning that every zoom, pan,
    // recentre and fix goes through this function -- which is true, and was
    // not enough, because the guard below returns before reaching it. With no
    // track loaded and nothing centred, a rider could zoom the route page,
    // come back to the dashboard, and find their framing gone: the zoom had
    // been applied to the view and never written to the shared camera, so the
    // dashboard restored whatever was there before. Intermittent, because it
    // depended entirely on whether a .gpx happened to be loaded.
    //
    // Nothing below this line touches the camera -- Redraw only reads it and
    // writes pixels -- so the top is as current as the bottom and reachable
    // from every path.
    // Gated: a hidden view still redraws (the dashboard's timer outlives its
    // own visibility, because the page is cached) and must not write the
    // camera the visible one owns. See MapView_t::camera_owner.
    if (view->camera_owner) {
        MapView_SaveCamera(view);
    }

    if (!view->have_center || GpxTrack_PointCount() == 0) {
        lv_line_set_points(view->trail, view->points, 0);
        if (view->trail_done != nullptr) {
            lv_line_set_points(view->trail_done, view->points, 0);
        }
        return;
    }

    // Projection and same-pixel collapsing both live in Map_BuildPolyline,
    // which test/host covers, so the tested code is the code that runs.
    // One projection for the frame, shared with the road layer through
    // MapView_HeadingDeg, so the two cannot disagree about which way is up.
    MapProjection_t proj;
    Map_PrepareProjection(&proj, view->center_lat, view->center_lon, view->metres_per_pixel,
                          (int16_t)(view->width / 2), (int16_t)(view->height / 2));
    Map_SetProjectionHeading(&proj, MapView_HeadingDeg(view));

    const size_t written =
        Map_BuildPolylinePrepared(GpxTrack_Buffer(), &proj, view->projected, view->capacity);

    for (size_t i = 0; i < written; i++) {
        view->points[i].x = view->projected[i].x;
        view->points[i].y = view->projected[i].y;
    }

    // Where to cut the drawn line: the vertex nearest the rider, measured in
    // pixels on the line that was actually drawn.
    //
    // Not a fraction of the source track, which is what this was. Since the
    // builder now draws only the visible stretch, and thins it when it does
    // not fit, source indices and drawn indices have no fixed relationship at
    // all -- the fraction put the colour change wherever it liked.
    // A different track means a different journey: a loaded route replacing
    // the old one must not inherit its progress.
    const size_t track_points = GpxTrack_PointCount();
    const char *track_name = GpxTrack_LoadedName();
    if (track_name == nullptr) {
        track_name = "";
    }
    if (view->done_src_track_points != track_points ||
        strncmp(view->done_src_track_name, track_name,
                sizeof(view->done_src_track_name) - 1) != 0) {
        view->done_src_track_points = track_points;
        strncpy(view->done_src_track_name, track_name,
                sizeof(view->done_src_track_name) - 1);
        view->done_src_track_name[sizeof(view->done_src_track_name) - 1] = '\0';
        view->done_src = 0;
        view->have_done_src = false;
    }

    // Advance the high-water mark, in source indices. Nearest-point rather
    // than anything cleverer: this only has to answer "how far along have we
    // ever been", and max() is what makes it monotonic.
    if (view->have_fix && track_points > 0) {
        const TrackBuffer_t *src = GpxTrack_Buffer();
        double best = 0.0;
        size_t nearest = 0;
        bool found = false;
        for (size_t i = 0; i < track_points; i++) {
            double lat = 0.0;
            double lon = 0.0;
            if (!TrackBuffer_Get(src, i, &lat, &lon)) {
                continue;
            }
            // Squared degrees, with longitude left unscaled. Good enough to
            // pick a vertex: the error it introduces is a cosine of latitude
            // on one axis, which cannot move the answer past a neighbouring
            // point at any spacing a track actually uses.
            const double dlat = lat - view->fix_lat;
            const double dlon = lon - view->fix_lon;
            const double d2 = (dlat * dlat) + (dlon * dlon);
            if (!found || d2 < best) {
                best = d2;
                nearest = i;
                found = true;
            }
        }
        // ⚠️ Only a fix that is actually ON the track may advance the mark.
        //
        // The mark is permanent by design, so one bad fix is permanent too: a
        // single valid-but-wrong position hundreds of metres away would mark
        // everything up to its nearest vertex as ridden, for the rest of the
        // route, with no way back. The old per-frame code had no memory and so
        // self-corrected on the next fix; buying monotonicity means buying
        // that risk, and this is the price of it.
        //
        // Degrees squared, compared against a budget converted at the equator
        // -- which under-reads longitude at higher latitudes and so is
        // conservative in the direction that matters: it rejects more, never
        // less.
        const double budget_deg = (double)MAP_DONE_MAX_OFFTRACK_M / 111320.0;
        const bool on_track = found && (best <= (budget_deg * budget_deg));
        if (on_track && (!view->have_done_src || nearest > view->done_src)) {
            view->done_src = nearest;
            view->have_done_src = true;
        }
    }

    // Where to cut the drawn line: the drawn vertex nearest the furthest
    // point reached, measured in pixels on the line that was actually drawn.
    //
    // Going through the source point rather than through the rider is what
    // makes this survive a turnaround, and it degrades correctly when that
    // point is off-screen: clipped off behind, the nearest drawn vertex is
    // the first one and everything visible is ahead; clipped off in front,
    // it is the last and everything visible is done.
    size_t split = 0;
    if (view->have_done_src && written > 0) {
        double dlat = 0.0;
        double dlon = 0.0;
        if (TrackBuffer_Get(GpxTrack_Buffer(), view->done_src, &dlat, &dlon)) {
            int16_t fx = 0;
            int16_t fy = 0;
            Map_ProjectPrepared(&proj, dlat, dlon, &fx, &fy);
            int32_t best = INT32_MAX;
            for (size_t i = 0; i < written; i++) {
                const int32_t dx = (int32_t)view->points[i].x - fx;
                const int32_t dy = (int32_t)view->points[i].y - fy;
                const int32_t d2 = dx * dx + dy * dy;
                if (d2 < best) {
                    best = d2;
                    split = i + 1;
                }
            }
        }
    }

    if (view->trail_done != nullptr) {
        // The two share the buffer and overlap by the point they meet at, so
        // the join has no gap in it.
        lv_line_set_points(view->trail_done, view->points, (uint16_t)split);
    }
    const size_t ahead_from = (split > 0) ? split - 1 : 0;
    lv_line_set_points(view->trail, view->points + ahead_from,
                       (uint16_t)(written - ahead_from));

    // Handed to the overlay rather than recomputed there: it decorates the
    // same geometry that was just drawn, and the drawn line is clipped and
    // thinned relative to the source.
    view->drawn_count = written;
    view->drawn_ahead_from = ahead_from;

    // The scale bar. Chosen from a round sequence so the label is a number a
    // rider can hold; see navigation/MapScale.h for why the distance leads and
    // the pixel length follows.
    {
        uint32_t metres = 0;
        int px = 0;
        if (MapScale_Choose(view->metres_per_pixel, SCALE_MAX_PX, &metres, &px)) {
            view->scale_px = px;
            if (view->scale_label != nullptr) {
                char buf[16];
                if (MapScale_Format(metres, buf, sizeof(buf))) {
                    lv_label_set_text(view->scale_label, buf);
                    lv_obj_align(view->scale_label, LV_ALIGN_BOTTOM_RIGHT, -HUD_MARGIN_PX,
                                 -(HUD_MARGIN_PX + 6));
                }
            }
        } else {
            view->scale_px = 0;
            if (view->scale_label != nullptr) {
                lv_label_set_text(view->scale_label, "");
            }
        }
    }

    // The save is at the TOP of this function, not here -- see the note there.
    // It is in Redraw at all, rather than at page teardown, because
    // PageManager runs the outgoing page's unload AFTER the incoming page's
    // will-appear: the dashboard would restore a camera the route page had not
    // saved yet, and the map would appear to reset itself.
}

bool MapView_IsTrackUp(const MapView_t *view) {
    return view != nullptr && Settings_GetMapTrackUp() && MapHeading_Valid(&view->heading);
}

double MapView_HeadingDeg(const MapView_t *view) {
    return MapView_IsTrackUp(view) ? MapHeading_Degrees(&view->heading) : 0.0;
}

double MapView_MetresAcross(const MapView_t *view) {
    if (view == nullptr) {
        return 0.0;
    }
    return view->metres_per_pixel * (double)view->width;
}

namespace {

// Metres per pixel, roughly halving each step. Spans 0.5km to 19km across a
// 240px panel: below that a rider is looking at their own front wheel, above
// it the detail filter has hidden everything worth seeing anyway.
// 1 m/px is 240m across this panel, ~35 seconds of riding at 25 km/h. That is
// the useful floor: close enough to read a complex junction, not so close that
// the next turn is off-screen before you reach it.
const double ZOOM_LADDER[] = {1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 80.0};
constexpr int ZOOM_STEPS = (int)(sizeof(ZOOM_LADDER) / sizeof(ZOOM_LADDER[0]));

// Nearest rung to where the view currently sits, so the first press after an
// automatic fit moves one visible step rather than jumping to an end.
int NearestStep(double mpp) {
    int best = 0;
    double best_d = 1e30;
    for (int i = 0; i < ZOOM_STEPS; i++) {
        const double d = mpp > ZOOM_LADDER[i] ? mpp / ZOOM_LADDER[i] : ZOOM_LADDER[i] / mpp;
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

void ApplyStep(MapView_t *view, int step) {
    if (step < 0 || step >= ZOOM_STEPS) {
        return;
    }
    view->metres_per_pixel = ZOOM_LADDER[step];
    view->zoom_locked = true;
    MapView_Redraw(view);
}

} // namespace

void MapView_ZoomIn(MapView_t *view) {
    if (view != nullptr) {
        ApplyStep(view, NearestStep(view->metres_per_pixel) - 1);
    }
}

void MapView_ZoomOut(MapView_t *view) {
    if (view != nullptr) {
        ApplyStep(view, NearestStep(view->metres_per_pixel) + 1);
    }
}

bool MapView_CanZoomIn(const MapView_t *view) {
    return view != nullptr && NearestStep(view->metres_per_pixel) > 0;
}

bool MapView_CanZoomOut(const MapView_t *view) {
    return view != nullptr && NearestStep(view->metres_per_pixel) < ZOOM_STEPS - 1;
}

void MapView_PanPixels(MapView_t *view, lv_coord_t dx, lv_coord_t dy) {
    if (view == nullptr || (dx == 0 && dy == 0)) {
        return;
    }
    if (!view->have_center) {
        return; // nothing to pan away from yet
    }

    const double mpp = view->metres_per_pixel;

    // The drag arrives in SCREEN pixels and the centre moves in map space, and
    // on a turned map those are not the same direction. Undo the view's
    // rotation first, or dragging north-east on a south-facing map walks the
    // centre south-west and the map runs away from the finger.
    //
    // This is the exact inverse of the rotation Map_ProjectPrepared applies.
    double mdx = dx;
    double mdy = dy;
    if (MapView_IsTrackUp(view)) {
        const double rad = MapView_HeadingDeg(view) * M_PI / 180.0;
        const double c = cos(rad);
        const double sn = sin(rad);
        mdx = dx * c - dy * sn;
        mdy = dx * sn + dy * c;
    }

    // y is inverted for the same reason Map_Project inverts it: screen y grows
    // downward while latitude grows north. Dragging down therefore walks the
    // centre north, which is what makes the map feel dragged rather than
    // scrolled.
    view->center_lat += (mdy * mpp) / MAP_EARTH_METRES_PER_DEGREE;

    double cos_lat = cos(view->center_lat * M_PI / 180.0);
    if (cos_lat < 0.01) {
        cos_lat = 0.01; // near the poles, where a degree of longitude vanishes
    }
    view->center_lon -= (mdx * mpp) / (MAP_EARTH_METRES_PER_DEGREE * cos_lat);

    view->pan_locked = true;
    MapView_Redraw(view);
}

void MapView_Recenter(MapView_t *view) {
    if (view == nullptr) {
        return;
    }
    view->pan_locked = false;
    view->zoom_locked = false;
    MapView_FitTrack(view);
}

namespace {

// Shared by every MapView_t, because it describes the map rather than a page.
//
// The scale and the centre are kept separately, and that separation is the
// point: a zoom is meaningful on its own, a centre is not. Storing them
// together meant a view with nothing centred saved neither, so a rider who
// zoomed before the map had anything to centre on lost the zoom silently.
double s_cam_mpp = 0.0;
bool s_cam_zoom_locked = false;
bool s_cam_pan_locked = false;
bool s_cam_have = false; // a scale has been saved

double s_cam_lat = 0.0;
double s_cam_lon = 0.0;
bool s_cam_have_centre = false; // ...and a centre with it

} // namespace

void MapView_SetCameraOwner(MapView_t *view, bool owner) {
    if (view != nullptr) {
        view->camera_owner = owner;
    }
}

void MapView_SaveCamera(const MapView_t *view) {
    if (view == nullptr) {
        return;
    }
    // Unconditional. The scale is what the rider set by hand and it survives
    // whether or not anything is centred yet.
    s_cam_mpp = view->metres_per_pixel;
    s_cam_zoom_locked = view->zoom_locked;
    s_cam_pan_locked = view->pan_locked;
    s_cam_have = true;

    if (view->have_center) {
        s_cam_lat = view->center_lat;
        s_cam_lon = view->center_lon;
        s_cam_have_centre = true;
    }
}

bool MapView_RestoreCamera(MapView_t *view) {
    if (view == nullptr || !s_cam_have || s_cam_mpp <= 0.0) {
        return false;
    }
    view->metres_per_pixel = s_cam_mpp;
    // The locks travel too. A rider who panned away from their fix expects it
    // to stay panned when they come back, and one who never touched the map
    // expects it to keep following them.
    view->zoom_locked = s_cam_zoom_locked;
    view->pan_locked = s_cam_pan_locked;

    if (s_cam_have_centre) {
        view->center_lat = s_cam_lat;
        view->center_lon = s_cam_lon;
        view->have_center = true;
    }

    MapView_Redraw(view);

    // ⚠️ True only when a CENTRE came back, not merely a scale. Page_Map reads
    // this to decide whether to fall back to MapView_FitTrack, and a scale
    // with no centre still needs that framing -- FitTrack respects the zoom
    // lock, so it will centre without undoing what the rider chose.
    return s_cam_have_centre;
}

void MapView_ForgetCamera() {
    s_cam_have = false;
    s_cam_have_centre = false;
}

bool MapView_IsManual(const MapView_t *view) {
    return view != nullptr && (view->pan_locked || view->zoom_locked);
}
