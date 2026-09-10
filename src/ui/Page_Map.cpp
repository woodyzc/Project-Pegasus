#include "Page_Map.h"

#include <stdio.h>

#include "../navigation/GpxTrack.h"
#include "../navigation/MapProject.h"
#include "../system/DataCenter.h"
#include "../system/PageManager/PageManager.h"
#include "../system/Settings.h"

// Shares Page_Dashboard's palette so the two screens read as one product.

namespace {

constexpr uint32_t COLOR_BG = 0x101820;
constexpr uint32_t COLOR_CAPTION = 0x93A4B8;
constexpr uint32_t COLOR_VALUE = 0xFFFFFF;
constexpr uint32_t COLOR_ACCENT = 0x61DAFB;
constexpr uint32_t COLOR_TRAIL = 0xFFD166;
constexpr uint32_t COLOR_PANEL = 0x18232E;

// Screen points handed to lv_line. 512 is far more than a 240x320 panel can
// resolve once same-pixel runs are collapsed, and costs 2KB.
constexpr size_t MAX_POLY_POINTS = 512;

constexpr int16_t MAP_W = 240;
constexpr int16_t MAP_H = 250; // below the header, above the footer
constexpr int16_t MAP_TOP = 40;
constexpr int16_t MAP_CX = MAP_W / 2;
constexpr int16_t MAP_CY = MAP_H / 2;

lv_obj_t *s_map_area = nullptr;
lv_obj_t *s_trail_line = nullptr;
lv_obj_t *s_here_dot = nullptr;
lv_obj_t *s_status_label = nullptr;
lv_obj_t *s_scale_label = nullptr;
lv_timer_t *s_refresh_timer = nullptr;

// lv_line keeps a pointer to this array rather than copying it, so it must
// outlive every redraw -- a local would be freed the moment the frame ended.
lv_point_t s_points[MAX_POLY_POINTS];
MapPoint_t s_projected[MAX_POLY_POINTS];

volatile bool s_gps_dirty = false;

// View state, only touched from the Core 1 refresh timer.
double s_center_lat = 0.0;
double s_center_lon = 0.0;
double s_metres_per_pixel = 5.0;
bool s_have_center = false;

void OnGpsPublished(const char *topic, const void *data, uint32_t size, void *user_arg) {
    (void)topic;
    (void)data;
    (void)size;
    (void)user_arg;
    s_gps_dirty = true;
}

Account s_gps_account("Page_Map/GPS", OnGpsPublished);

// Rebuilds the polyline from the loaded track at the current view.
void RebuildPolyline() {
    if (!s_have_center || GpxTrack_PointCount() == 0) {
        lv_line_set_points(s_trail_line, s_points, 0);
        return;
    }

    // The projection and the same-pixel collapsing are Map_BuildPolyline's,
    // which test/host covers -- so the tested code is the code that runs
    // rather than a second copy of the same loop living here.
    const size_t written =
        Map_BuildPolyline(GpxTrack_Buffer(), s_center_lat, s_center_lon, s_metres_per_pixel,
                          MAP_CX, MAP_CY, s_projected, MAX_POLY_POINTS);

    for (size_t i = 0; i < written; i++) {
        s_points[i].x = s_projected[i].x;
        s_points[i].y = s_projected[i].y;
    }
    lv_line_set_points(s_trail_line, s_points, (uint16_t)written);

    // Scale bar as a number rather than a ruler: 240px of panel does not spare
    // room for one, and a figure is unambiguous.
    const double across_m = s_metres_per_pixel * MAP_W;
    if (across_m >= 1000.0) {
        lv_label_set_text_fmt(s_scale_label, "%.1f km across", across_m / 1000.0);
    } else {
        lv_label_set_text_fmt(s_scale_label, "%d m across", (int)across_m);
    }
}

void RefreshTimerCallback(lv_timer_t *timer) {
    (void)timer;

    if (!s_gps_dirty) {
        return;
    }
    s_gps_dirty = false;

    GPS_Info_t gps;
    if (!DataCenter_Pull(TOPIC_GPS_INFO, &gps, sizeof(gps))) {
        return;
    }

    if (!gps.fix_valid) {
        // Keep showing the trail centred on itself; only the rider marker is
        // meaningless without a fix.
        lv_obj_add_flag(s_here_dot, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_status_label, "Waiting for fix");
        lv_obj_set_style_text_color(s_status_label, lv_color_hex(COLOR_CAPTION), 0);
        return;
    }

    // Once there is a fix the view follows the rider rather than the trail's
    // bounding box: what matters while riding is where you are on the line,
    // not the shape of the whole route.
    s_center_lat = gps.lat;
    s_center_lon = gps.lon;
    s_have_center = true;

    lv_obj_clear_flag(s_here_dot, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(s_status_label, "%d sats", (int)gps.num_sv);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(COLOR_ACCENT), 0);

    RebuildPolyline();
}

void OnBackClicked(lv_event_t *e) {
    PageMap *self = (PageMap *)lv_event_get_user_data(e);
    if (self != nullptr && self->_Manager != nullptr) {
        self->_Manager->Pop();
    }
}

} // namespace

void PageMap::onViewLoad() {
    lv_obj_t *parent = _root;
    lv_obj_set_style_bg_color(parent, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Header ----
    lv_obj_t *back_btn = lv_btn_create(parent);
    lv_obj_set_size(back_btn, 40, 32);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 6, 4);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(COLOR_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_radius(back_btn, 8, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_add_event_cb(back_btn, OnBackClicked, LV_EVENT_CLICKED, this);

    lv_obj_t *back_icon = lv_label_create(back_btn);
    lv_label_set_text(back_icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_icon, lv_color_hex(COLOR_VALUE), 0);
    lv_obj_center(back_icon);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "ROUTE");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_CAPTION), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    s_status_label = lv_label_create(parent);
    lv_label_set_text(s_status_label, "No fix");
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(COLOR_CAPTION), 0);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_RIGHT, -10, 14);

    // ---- Map area ----
    s_map_area = lv_obj_create(parent);
    lv_obj_set_size(s_map_area, MAP_W, MAP_H);
    lv_obj_align(s_map_area, LV_ALIGN_TOP_MID, 0, MAP_TOP);
    lv_obj_set_style_bg_color(s_map_area, lv_color_hex(0x0B1116), 0);
    lv_obj_set_style_border_width(s_map_area, 0, 0);
    lv_obj_set_style_radius(s_map_area, 0, 0);
    lv_obj_set_style_pad_all(s_map_area, 0, 0);
    // Clip rather than scroll: a trail point can project far outside the
    // viewport, and without clipping LVGL would grow a scrollable area around
    // it and let the user drag the map into empty space.
    lv_obj_clear_flag(s_map_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_clip_corner(s_map_area, true, 0);

    s_trail_line = lv_line_create(s_map_area);
    lv_obj_set_style_line_color(s_trail_line, lv_color_hex(COLOR_TRAIL), 0);
    lv_obj_set_style_line_width(s_trail_line, 2, 0);
    lv_obj_set_style_line_rounded(s_trail_line, true, 0);
    lv_obj_set_pos(s_trail_line, 0, 0);

    // The rider: a fixed dot at the centre, because the map moves under it.
    s_here_dot = lv_obj_create(s_map_area);
    lv_obj_set_size(s_here_dot, 10, 10);
    lv_obj_set_style_radius(s_here_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_here_dot, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(s_here_dot, 2, 0);
    lv_obj_set_style_border_color(s_here_dot, lv_color_hex(COLOR_BG), 0);
    lv_obj_clear_flag(s_here_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_here_dot, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_here_dot, LV_OBJ_FLAG_HIDDEN); // until there is a fix

    // ---- Footer ----
    s_scale_label = lv_label_create(parent);
    lv_obj_set_style_text_font(s_scale_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_scale_label, lv_color_hex(COLOR_CAPTION), 0);
    lv_obj_align(s_scale_label, LV_ALIGN_BOTTOM_LEFT, 10, -8);
    lv_label_set_text(s_scale_label, "");

    lv_obj_t *name = lv_label_create(parent);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(COLOR_CAPTION), 0);
    lv_obj_set_width(name, 130);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(name, LV_ALIGN_BOTTOM_RIGHT, -10, -8);

    // ---- Initial view: frame the whole trail until a fix arrives ----
    if (GpxTrack_PointCount() > 0 && GpxTrack_Center(&s_center_lat, &s_center_lon)) {
        s_have_center = true;

        // Fit from the trail's own bounds, so the first look shows the whole
        // route rather than an arbitrary zoom. The bounds cover every point
        // the file held, including those thinning discarded.
        double min_lat;
        double max_lat;
        double min_lon;
        double max_lon;
        if (GpxTrack_Bounds(&min_lat, &max_lat, &min_lon, &max_lon)) {
            s_metres_per_pixel = Map_FitScale(min_lat, max_lat, min_lon, max_lon, MAP_W, MAP_H, 12);
        }

        lv_label_set_text(name, GpxTrack_LoadedName());
        RebuildPolyline();
    } else {
        // Say which of the two reasons applies -- a missing card and an
        // unreadable file need different fixes from the rider.
        lv_label_set_text(name, GpxTrack_CardMounted() ? "No .gpx on card" : "No SD card");
        lv_label_set_text(s_scale_label, "");
    }

    DataCenter_Subscribe(TOPIC_GPS_INFO, &s_gps_account);
    s_refresh_timer = lv_timer_create(RefreshTimerCallback, 500, nullptr);
}

void PageMap::onViewUnload() {
    if (s_refresh_timer != nullptr) {
        lv_timer_del(s_refresh_timer);
        s_refresh_timer = nullptr;
    }
    DataCenter_Unsubscribe(TOPIC_GPS_INFO, &s_gps_account);

    s_map_area = nullptr;
    s_trail_line = nullptr;
    s_here_dot = nullptr;
    s_status_label = nullptr;
    s_scale_label = nullptr;
    s_have_center = false;
}
