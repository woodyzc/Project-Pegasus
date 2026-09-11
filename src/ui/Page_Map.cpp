#include "Page_Map.h"

#include "../hal/LvglFs.h"

#include "../navigation/GpxTrack.h"
#include "../system/DataCenter.h"
#include "../system/PageManager/PageManager.h"
#include "MapView.h"

// The same breadcrumb map the dashboard shows inline, given the whole panel.
// Nearly twice the area, which is the difference between reading the trail at
// a junction and squinting at it.
//
// The drawing is MapView's; this file is only the page around it -- header,
// satellite count, scale and file name. The projection deliberately does not
// live here: it lived here once, in parallel with the dashboard's copy, and a
// fix to one would have missed the other.

namespace {

constexpr uint32_t COLOR_BG = 0x101820;
constexpr uint32_t COLOR_CAPTION = 0x93A4B8;
constexpr uint32_t COLOR_VALUE = 0xFFFFFF;
constexpr uint32_t COLOR_ACCENT = 0x61DAFB;
constexpr uint32_t COLOR_PANEL = 0x18232E;

constexpr lv_coord_t MAP_X = 0;
constexpr lv_coord_t MAP_Y = 36;
constexpr lv_coord_t MAP_W = 240;
constexpr lv_coord_t MAP_H = 262;

// The one tile the spike draws. Matches tools/tilegen.py's defaults, so
// `tilegen.py synth /tmp/MAP` then copying that tree to the card is all the
// setup there is.
#define TILE_SPIKE_PATH "/15/8721/12556.bin"

lv_obj_t *s_tile_img = nullptr;
lv_obj_t *s_tile_stat = nullptr;

// Twice the dashboard's allowance, because this view has roughly twice the
// area to resolve. Its own arrays rather than the dashboard's: during a page
// transition both maps are on screen at once, so sharing would corrupt one
// mid-slide.
constexpr size_t MAX_POLY_POINTS = 512;
lv_point_t s_points[MAX_POLY_POINTS];
MapPoint_t s_projected[MAX_POLY_POINTS];

MapView_t s_view;
lv_obj_t *s_status_label = nullptr;
lv_obj_t *s_scale_label = nullptr;
lv_timer_t *s_refresh_timer = nullptr;

volatile bool s_gps_dirty = false;

void OnGpsPublished(const char *topic, const void *data, uint32_t size, void *user_arg) {
    (void)topic;
    (void)data;
    (void)size;
    (void)user_arg;
    s_gps_dirty = true;
}

Account s_gps_account("Page_Map/GPS", OnGpsPublished);

void UpdateScale() {
    if (s_scale_label == nullptr) {
        return;
    }
    // A figure rather than a ruler: 240px of panel does not spare the room,
    // and a number is unambiguous.
    const double across = MapView_MetresAcross(&s_view);
    if (across >= 1000.0) {
        lv_label_set_text_fmt(s_scale_label, "%.1f km across", across / 1000.0);
    } else {
        lv_label_set_text_fmt(s_scale_label, "%d m across", (int)across);
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
        // The trail stays drawn, framed on itself -- only the rider marker is
        // meaningless without a fix, and MapView hides it.
        MapView_SetPosition(&s_view, &gps);
        lv_label_set_text(s_status_label, "Waiting for fix");
        lv_obj_set_style_text_color(s_status_label, lv_color_hex(COLOR_CAPTION), 0);
        return;
    }

    MapView_SetPosition(&s_view, &gps);
    lv_label_set_text_fmt(s_status_label, "%d sats", (int)gps.num_sv);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(COLOR_ACCENT), 0);
    UpdateScale();
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
    lv_obj_set_size(back_btn, 36, 26);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 6, 4);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(COLOR_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_radius(back_btn, 6, 0);
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
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    s_status_label = lv_label_create(parent);
    lv_label_set_text(s_status_label, "No fix");
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(COLOR_CAPTION), 0);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_RIGHT, -8, 12);

    // ---- Spike: one tile behind the track ----
    // Step 2 of the offline-map scope, and deliberately the stupidest version
    // that can answer anything: a hardcoded path, no projection, no cache, no
    // position. It exists to establish two facts that everything downstream
    // assumes -- that LVGL can read an image off this card at all, and how
    // long one 128KB tile actually takes on this board's SD bus.
    //
    // Created BEFORE MapView so the track draws on top of it.
    if (LvglFs_IsReady()) {
        s_tile_img = lv_img_create(parent);
        lv_img_set_src(s_tile_img, "S:" TILE_SPIKE_PATH);
        lv_obj_set_pos(s_tile_img, MAP_X, MAP_Y);
        // The tile is 256 square and the window is 240x262, so it is clipped
        // rather than scaled -- scaling would make the read time meaningless.
        lv_obj_add_flag(s_tile_img, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(s_tile_img, LV_OBJ_FLAG_SCROLLABLE);
    }

    // ---- The map ----
    MapView_Create(&s_view, parent, MAP_X, MAP_Y, MAP_W, MAP_H, s_points, s_projected,
                   MAX_POLY_POINTS);

    // ---- Footer ----
    s_scale_label = lv_label_create(parent);
    lv_obj_set_style_text_font(s_scale_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_scale_label, lv_color_hex(COLOR_CAPTION), 0);
    lv_obj_align(s_scale_label, LV_ALIGN_BOTTOM_LEFT, 8, -4);
    lv_label_set_text(s_scale_label, "");

    // The measurement, on the panel, because serial cannot carry it. Reports
    // what the tile above cost: bytes, milliseconds, and the implied rate.
    s_tile_stat = lv_label_create(parent);
    lv_obj_set_style_text_font(s_tile_stat, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_tile_stat, lv_color_hex(0x61DAFB), 0);
    lv_obj_align(s_tile_stat, LV_ALIGN_TOP_LEFT, 8, 26);
    if (!LvglFs_IsReady()) {
        lv_label_set_text(s_tile_stat, "tile: no fs driver");
    } else if (LvglFs_LastReadBytes() == 0) {
        lv_label_set_text(s_tile_stat, "tile: not found");
    } else {
        const uint32_t us = LvglFs_LastReadUs();
        const uint32_t bytes = LvglFs_LastReadBytes();
        lv_label_set_text_fmt(s_tile_stat, "tile %u B in %u ms = %.2f MB/s",
                              (unsigned)bytes, (unsigned)(us / 1000),
                              (double)bytes / (double)us);
    }

    lv_obj_t *name = lv_label_create(parent);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(COLOR_CAPTION), 0);
    lv_obj_set_width(name, 140);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(name, LV_ALIGN_BOTTOM_RIGHT, -8, -4);

    if (GpxTrack_PointCount() > 0) {
        // Frame the whole trail until a fix arrives, so the first look shows
        // the route rather than an arbitrary zoom.
        MapView_FitTrack(&s_view);
        lv_label_set_text(name, GpxTrack_LoadedName());
        UpdateScale();
    } else {
        // Say which of the two reasons applies: a missing card and an
        // unreadable one need different things from the rider.
        lv_label_set_text(name, GpxTrack_CardMounted() ? "No .gpx on card" : "No SD card");
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

    s_status_label = nullptr;
    s_scale_label = nullptr;
}
