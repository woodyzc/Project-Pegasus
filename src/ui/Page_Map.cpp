#include "Page_Map.h"

#include <Arduino.h>


#include "../navigation/GpxTrack.h"
#include "../navigation/MapProject.h"
#include "../navigation/RoadMap.h"
#include "../system/DataCenter.h"
#include "../system/PageManager/PageManager.h"
#include "MapView.h"
#include "RoadView.h"

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

lv_obj_t *s_road_stat = nullptr;
lv_timer_t *s_road_timer = nullptr;

// Reports the road map: ways loaded, segments drawn of points held, and the
// the path it tried: "not found" on its own sent the first bring-up looking in
// the wrong place, when the answer was one directory away.
void RoadStatTimer(lv_timer_t *timer) {
    (void)timer;
    if (s_road_stat == nullptr) {
        return;
    }
    if (RoadMap_IsLoaded()) {
        // Visible ways matter as much as segments: zero visible with a map
        // loaded means the roads are for somewhere else, which looks identical
        // to a broken renderer until the number is on screen.
        lv_label_set_text_fmt(s_road_stat, "%u/%u ways, %u seg, %u us",
                              (unsigned)RoadView_LastVisibleWays(),
                              (unsigned)RoadMap_WayCount(),
                              (unsigned)RoadView_LastSegments(),
                              (unsigned)RoadView_LastDrawUs());
    } else {
        lv_label_set_text(s_road_stat, "no /MAP/roads.prd on the card");
    }
}

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

void OnZoomClicked(lv_event_t *e) {
    const int in = (int)(intptr_t)lv_event_get_user_data(e);
    if (in == 0) {
        MapView_ZoomIn(&s_view);
    } else {
        MapView_ZoomOut(&s_view);
    }
    // The roads are drawn from the view's scale, and LVGL has no idea this
    // changed it.
    RoadView_Refresh();
    UpdateScale();
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
    RoadView_Refresh();
        RoadView_Refresh();
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

    // ---- The map ----
    MapView_Create(&s_view, parent, MAP_X, MAP_Y, MAP_W, MAP_H, s_points, s_projected,
                   MAX_POLY_POINTS);

    // Roads behind the trail, sharing this view's projection.
    RoadView_Attach(&s_view);


    // ---- Zoom ----
    // Right edge, stacked, deliberately large. This is the one control on the
    // map and it is pressed with a thumb, possibly gloved, possibly moving.
    // Small round buttons would be the obvious design and the wrong one.
    {
        static const char *const LABEL[2] = {LV_SYMBOL_PLUS, LV_SYMBOL_MINUS};
        for (int i = 0; i < 2; i++) {
            lv_obj_t *btn = lv_btn_create(parent);
            lv_obj_set_size(btn, 44, 44);
            lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -6, MAP_Y + 16 + i * 52);
            lv_obj_set_style_radius(btn, 8, 0);
            lv_obj_set_style_shadow_width(btn, 0, 0);
            // Semi-transparent: it sits over the map, and a solid button
            // would punch a hole in exactly the thing being navigated.
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x101820), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_70, 0);
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x61DAFB), LV_STATE_PRESSED);
            lv_obj_add_event_cb(btn, OnZoomClicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

            lv_obj_t *label = lv_label_create(btn);
            lv_label_set_text(label, LABEL[i]);
            lv_obj_set_style_text_color(label, lv_color_hex(COLOR_VALUE), 0);
            lv_obj_center(label);
        }
    }

    // ---- Footer ----
    s_scale_label = lv_label_create(parent);
    lv_obj_set_style_text_font(s_scale_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_scale_label, lv_color_hex(COLOR_CAPTION), 0);
    lv_obj_align(s_scale_label, LV_ALIGN_BOTTOM_LEFT, 8, -4);
    lv_label_set_text(s_scale_label, "");

    // The measurement, on the panel, because serial cannot carry it.
    s_road_stat = lv_label_create(parent);
    lv_obj_set_style_text_font(s_road_stat, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_road_stat, lv_color_hex(0x61DAFB), 0);
    lv_obj_set_width(s_road_stat, 224);
    lv_label_set_long_mode(s_road_stat, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_road_stat, LV_ALIGN_TOP_LEFT, 8, 26);
    lv_label_set_text(s_road_stat, "roads: waiting");

    // On a timer: the draw figure only exists after the first draw, which has
    // not happened while this page is still being built.
    s_road_timer = lv_timer_create(RoadStatTimer, 500, nullptr);
    RoadStatTimer(nullptr);

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
        RoadView_Refresh();
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
    // Same reasoning as the refresh timer above: it outlives the widgets
    // unless torn down here, and would write to freed lv_obj pointers on its
    // next tick.
    if (s_road_timer != nullptr) {
        lv_timer_del(s_road_timer);
        s_road_timer = nullptr;
    }
    DataCenter_Unsubscribe(TOPIC_GPS_INFO, &s_gps_account);

    s_status_label = nullptr;
    s_scale_label = nullptr;
    s_road_stat = nullptr;
}
