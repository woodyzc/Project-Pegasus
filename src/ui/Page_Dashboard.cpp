#include "Page_Dashboard.h"

#include <math.h>

#include "../system/DataCenter.h"

namespace {

lv_obj_t *s_speed_meter = nullptr;
lv_meter_indicator_t *s_speed_needle = nullptr;
lv_obj_t *s_speed_label = nullptr;
lv_obj_t *s_hr_label = nullptr;
lv_obj_t *s_slope_label = nullptr;
lv_obj_t *s_battery_label = nullptr;

// Set by the DataCenter callbacks below (which may run on Core 0 -- see
// CLAUDE.md's "no direct LVGL access from Core 0" rule) and consumed by
// RefreshTimerCallback, which only ever runs on Core 1 via lv_timer_handler().
// `volatile` is enough here: each flag has one writer (its DataCenter
// callback) and one reader (the refresh timer), and a torn/stale read at
// worst delays a widget update by one 100ms refresh tick, never corrupts
// state -- a mutex would just add overhead for no correctness benefit here
// (unlike DataCenter's own cross-topic table, which genuinely needs one).
volatile bool s_gps_dirty = false;
volatile bool s_hr_dirty = false;

// DataCenter callback -- may run on Core 0 (whichever core published). Must
// NOT touch any LVGL object; only ever set a flag for the Core-1 refresh
// timer to pick up.
void OnGpsPublished(const char *topic, const void *data, uint32_t size, void *user_arg) {
    (void)topic;
    (void)data;
    (void)size;
    (void)user_arg;
    s_gps_dirty = true;
}

void OnHeartRatePublished(const char *topic, const void *data, uint32_t size, void *user_arg) {
    (void)topic;
    (void)data;
    (void)size;
    (void)user_arg;
    s_hr_dirty = true;
}

// The only place in this file allowed to touch LVGL objects: an lv_timer
// callback runs exclusively from lv_timer_handler(), which this project only
// ever calls from lvgl_task() on Core 1 (see src/system/LvglTask.cpp).
void RefreshTimerCallback(lv_timer_t *timer) {
    (void)timer;

    if (s_gps_dirty) {
        s_gps_dirty = false;
        GPS_Info_t gps;
        if (DataCenter_Pull(TOPIC_GPS_INFO, &gps, sizeof(gps))) {
            float speed_kmh = gps.speed * 3.6f;
            lv_meter_set_indicator_value(s_speed_meter, s_speed_needle, (int32_t)lroundf(speed_kmh));
            lv_label_set_text_fmt(s_speed_label, "%.1f km/h", speed_kmh);
        }
    }

    if (s_hr_dirty) {
        s_hr_dirty = false;
        HeartRate_t hr;
        if (DataCenter_Pull(TOPIC_HEART_RATE, &hr, sizeof(hr))) {
            lv_label_set_text_fmt(s_hr_label, LV_SYMBOL_CHARGE " %d bpm", hr.bpm);
        }
    }
}

} // namespace

void Page_Dashboard_Create(lv_obj_t *parent) {
    if (parent == nullptr) {
        parent = lv_scr_act();
    }
    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);

    // ---- Speed meter ----
    s_speed_meter = lv_meter_create(parent);
    lv_obj_set_size(s_speed_meter, 200, 200);
    lv_obj_align(s_speed_meter, LV_ALIGN_TOP_MID, 0, 10);

    lv_meter_scale_t *scale = lv_meter_add_scale(s_speed_meter);
    lv_meter_set_scale_ticks(s_speed_meter, scale, 21, 2, 10, lv_palette_main(LV_PALETTE_GREY));
    lv_meter_set_scale_major_ticks(s_speed_meter, scale, 4, 4, 15, lv_color_white(), 10);
    lv_meter_set_scale_range(s_speed_meter, scale, 0, 60, 270, 135); // 0-60 km/h: a plausible bike speed range, not a calibrated value

    s_speed_needle = lv_meter_add_needle_line(s_speed_meter, scale, 4, lv_palette_main(LV_PALETTE_RED), -10);

    s_speed_label = lv_label_create(s_speed_meter);
    lv_obj_set_style_text_font(s_speed_label, &lv_font_montserrat_24, 0);
    lv_obj_align(s_speed_label, LV_ALIGN_CENTER, 0, 40);
    lv_label_set_text(s_speed_label, "-- km/h");

    // ---- Heart rate ----
    s_hr_label = lv_label_create(parent);
    lv_obj_set_style_text_font(s_hr_label, &lv_font_montserrat_24, 0);
    lv_obj_align(s_hr_label, LV_ALIGN_TOP_MID, 0, 220);
    lv_label_set_text(s_hr_label, LV_SYMBOL_CHARGE " -- bpm");

    // ---- Slope icon ----
    // Placeholder: this task's Prompt only asks to subscribe to GPS_Info and
    // Sensor/HeartRate, so there's no wired data source for grade/slope yet
    // (candidates: Sensor/IMU's pitch, or a GPS-altitude-derived grade calc
    // -- neither implemented here). Widget exists per the "create a slope
    // icon" ask; it just doesn't update yet.
    s_slope_label = lv_label_create(parent);
    lv_obj_set_style_text_font(s_slope_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_slope_label, LV_ALIGN_BOTTOM_LEFT, 10, -10);
    lv_label_set_text(s_slope_label, LV_SYMBOL_UP " --%");

    // ---- Battery indicator ----
    // Placeholder for the same reason -- this is the device's own battery
    // level, which has no DataCenter topic yet (not to be confused with
    // HeartRate_t.battery, which is the HR strap's battery).
    s_battery_label = lv_label_create(parent);
    lv_obj_set_style_text_font(s_battery_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_battery_label, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_label_set_text(s_battery_label, LV_SYMBOL_BATTERY_FULL " --%");

    // ---- DataCenter subscriptions ----
    // Static: one Account per subscription, living for the program's
    // lifetime (Page_Dashboard is never destroyed in the current design).
    static Account gps_account("Page_Dashboard/GPS", OnGpsPublished);
    static Account hr_account("Page_Dashboard/HeartRate", OnHeartRatePublished);
    DataCenter_Subscribe(TOPIC_GPS_INFO, &gps_account);
    DataCenter_Subscribe(TOPIC_HEART_RATE, &hr_account);

    lv_timer_create(RefreshTimerCallback, 100, nullptr);
}
