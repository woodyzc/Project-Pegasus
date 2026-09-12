#include "MapView.h"

#include <math.h>

#include "../navigation/GpxTrack.h"

namespace {

constexpr uint32_t COLOR_MAP_BG = 0x0B1116;
constexpr uint32_t COLOR_TRAIL = 0xFFD166;
constexpr uint32_t COLOR_MARKER = 0x61DAFB;

// Half-height of the heading triangle. Big enough to read the direction at a
// glance, small enough not to hide the trail underneath it.
constexpr double MARKER_RADIUS = 7.0;

} // namespace

void MapView_Create(MapView_t *view, lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                    lv_coord_t h, lv_point_t *points, MapPoint_t *projected, size_t capacity) {
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

    view->trail = lv_line_create(view->container);
    lv_obj_set_style_line_color(view->trail, lv_color_hex(COLOR_TRAIL), 0);
    lv_obj_set_style_line_width(view->trail, 2, 0);
    lv_obj_set_style_line_rounded(view->trail, true, 0);
    lv_obj_set_pos(view->trail, 0, 0);

    // A triangle rather than a dot: the rider's heading is already published
    // and carries real information at a junction -- which way am I pointing
    // relative to the line I am supposed to be following.
    view->marker = lv_line_create(view->container);
    lv_obj_set_style_line_color(view->marker, lv_color_hex(COLOR_MARKER), 0);
    lv_obj_set_style_line_width(view->marker, 2, 0);
    lv_obj_set_style_line_rounded(view->marker, true, 0);
    lv_obj_set_pos(view->marker, 0, 0);
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

    // Once there is a fix the view follows the rider: what matters while
    // riding is where you are on the line, not the shape of the whole route.
    view->center_lat = gps->lat;
    view->center_lon = gps->lon;
    view->have_center = true;

    {
        // Heading is degrees clockwise from north, so it maps to screen with
        // sin on x and -cos on y, matching the projection's own convention.
        const double rad = gps->heading * M_PI / 180.0;
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

        lv_line_set_points(view->marker, view->marker_points, 4);
        lv_obj_clear_flag(view->marker, LV_OBJ_FLAG_HIDDEN);
    }

    MapView_Redraw(view);
}

void MapView_Redraw(MapView_t *view) {
    if (view == nullptr || view->trail == nullptr) {
        return;
    }

    if (!view->have_center || GpxTrack_PointCount() == 0) {
        lv_line_set_points(view->trail, view->points, 0);
        return;
    }

    // Projection and same-pixel collapsing both live in Map_BuildPolyline,
    // which test/host covers, so the tested code is the code that runs.
    const size_t written = Map_BuildPolyline(GpxTrack_Buffer(), view->center_lat, view->center_lon,
                                             view->metres_per_pixel, (int16_t)(view->width / 2),
                                             (int16_t)(view->height / 2), view->projected,
                                             view->capacity);

    for (size_t i = 0; i < written; i++) {
        view->points[i].x = view->projected[i].x;
        view->points[i].y = view->projected[i].y;
    }
    lv_line_set_points(view->trail, view->points, (uint16_t)written);
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
const double ZOOM_LADDER[] = {2.0, 4.0, 8.0, 16.0, 32.0, 80.0};
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
