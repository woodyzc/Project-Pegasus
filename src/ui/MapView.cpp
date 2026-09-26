#include "MapView.h"

#include <math.h>
#include <stdint.h>

#include "../navigation/GpxTrack.h"
#include "../system/Settings.h"

namespace {

constexpr uint32_t COLOR_MAP_BG = 0x0B1116;
// The trail, in two colours: what is behind the rider and what is ahead.
//
// Green ahead and amber behind rather than the other way round. The part that
// matters at a glance is the part still to ride, and green reads as "this way"
// where amber reads as a mark left behind -- which is what it is.
constexpr uint32_t COLOR_TRAIL_DONE = 0xFFD166;
constexpr uint32_t COLOR_TRAIL_AHEAD = 0x7CE38B;

constexpr uint32_t COLOR_MARKER = 0x61DAFB;

// Outlined in the map's own background colour, so the marker keeps a hard
// edge wherever it sits. It is drawn ON TOP of the route and the roads, and
// every one of those is light -- white-grey streets, green and amber trail --
// so a cyan triangle laid directly on them loses its shape at exactly the
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
void MarkerDrawCb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    MapView_t *view = (MapView_t *)lv_obj_get_user_data(obj);
    if (view == nullptr) {
        return;
    }
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);

    lv_area_t area;
    lv_obj_get_coords(obj, &area);

    // marker_points[0..2] are the corners; [3] repeats [0] to close the
    // outline stroke below.
    const lv_point_t *p = view->marker_points;

    lv_coord_t min_y = p[0].y;
    lv_coord_t max_y = p[0].y;
    for (int i = 1; i < 3; i++) {
        if (p[i].y < min_y) min_y = p[i].y;
        if (p[i].y > max_y) max_y = p[i].y;
    }

    // Rects rather than 1px lines: a horizontal line is anti-aliased at both
    // ends, so a stack of them builds a shape with soft ragged sides. A rect
    // of one row is exact, and the outline below supplies the smooth edge.
    lv_draw_rect_dsc_t fill;
    lv_draw_rect_dsc_init(&fill);
    fill.bg_color = lv_color_hex(COLOR_MARKER);
    fill.bg_opa = LV_OPA_COVER;
    fill.border_width = 0;
    fill.radius = 0;

    for (lv_coord_t y = min_y; y <= max_y; y++) {
        // Where this row crosses each edge. A triangle is convex, so the
        // filled span is simply the leftmost crossing to the rightmost.
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
            xs[n++] = (double)a.x + (double)(b.x - a.x) * (double)(y - a.y) /
                                        (double)(b.y - a.y);
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

    view->container = lv_obj_create(parent);
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
    MapView_SaveCamera(view);

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
    size_t split = 0;
    if (view->have_fix && written > 0) {
        int16_t fx = 0;
        int16_t fy = 0;
        Map_ProjectPrepared(&proj, view->fix_lat, view->fix_lon, &fx, &fy);
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

    if (view->trail_done != nullptr) {
        // The two share the buffer and overlap by the point they meet at, so
        // the join has no gap in it.
        lv_line_set_points(view->trail_done, view->points, (uint16_t)split);
    }
    const size_t ahead_from = (split > 0) ? split - 1 : 0;
    lv_line_set_points(view->trail, view->points + ahead_from,
                       (uint16_t)(written - ahead_from));

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
