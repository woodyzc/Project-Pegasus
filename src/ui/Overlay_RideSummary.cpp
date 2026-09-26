#include "Overlay_RideSummary.h"

#include <lvgl.h>
#include <stdio.h>
#include <string.h>

#include "../navigation/RideLog.h"
#include "../system/HrZone.h"
#include "../system/RideStats.h"
#include "../system/Settings.h"
#include "../system/Trip.h"

namespace {

constexpr uint32_t COLOR_BG = 0x0D1720;
constexpr uint32_t COLOR_PANEL = 0x141E27;
constexpr uint32_t COLOR_CAPTION = 0x93A4B8;
constexpr uint32_t COLOR_VALUE = 0xFFFFFF;
constexpr uint32_t COLOR_ACCENT = 0x61DAFB;
constexpr uint32_t COLOR_RULE = 0x24313D;

// Five zone colours, the same table the dashboard uses, so a heart rate means
// the same thing here as it did while the rider was looking at it.
constexpr uint32_t ZONE_COLORS[] = {0x0A84FF, 0x30D158, 0xFFD60A, 0xFF3B30, 0xBF5AF2};

lv_obj_t *s_root = nullptr;

void OnCloseClicked(lv_event_t *e) {
    (void)e;
    if (s_root == nullptr) {
        return;
    }
    // Async, because the button that fired this event is a child of the object
    // being deleted. LVGL keeps using the event target after the handler
    // returns, so freeing its ancestor here frees the ground it is standing
    // on; lv_obj_del_async exists for exactly this and defers to the end of
    // the current lv_timer_handler pass.
    lv_obj_del_async(s_root);
    // Cleared now rather than when the delete lands: nothing may touch it in
    // between, and Show() must see that the overlay is on its way out.
    s_root = nullptr;
}

// One line of the report: a small word on the left, the figure on the right.
//
// Deliberately the same shape as the dashboard's AVG/MAX rows. The rider has
// been reading that pairing for the whole ride, and a summary that invented a
// new arrangement for the same numbers would be a second thing to learn.
void Row(lv_obj_t *parent, const char *word, const char *value, uint32_t color) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 26);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, word);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_CAPTION), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *figure = lv_label_create(row);
    lv_label_set_text(figure, value);
    lv_obj_set_style_text_font(figure, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(figure, lv_color_hex(color), 0);
    lv_obj_align(figure, LV_ALIGN_RIGHT_MID, 0, 0);
}

} // namespace

void RideSummary_Capture(RideSummary_t *out) {
    if (out == nullptr) {
        return;
    }
    memset(out, 0, sizeof(*out));

    out->distance_km = Trip_Km();
    out->moving_seconds = RideStats_MovingSeconds();
    out->avg_kmh = RideStats_AvgSpeedKmh();
    out->max_kmh = RideStats_MaxSpeedKmh();
    out->avg_bpm = RideStats_AvgBpm();
    out->max_bpm = RideStats_MaxBpm();
    out->ascent_m = RideStats_AscentM();
    out->points = RideLog_PointCount();

    RideLog_FileName(out->file, sizeof(out->file));
}

void Overlay_RideSummary_Show(const RideSummary_t *summary, bool ended) {
    if (s_root != nullptr || summary == nullptr) {
        return;
    }

    s_root = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_root, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 12, 0);
    lv_obj_set_style_pad_row(s_root, 2, 0);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *title = lv_label_create(s_root);
    lv_label_set_text(title, ended ? "RIDE ENDED" : "RIDE SO FAR");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_text_letter_space(title, 1, 0);

    char buf[64];

    snprintf(buf, sizeof(buf), "%.2f %s", (double)Settings_DistanceFromKm(summary->distance_km),
             Settings_DistanceUnitLabel());
    Row(s_root, "DISTANCE", buf, COLOR_VALUE);

    char time_text[RIDE_SUMMARY_TIME_MAX];
    if (!RideSummary_FormatDuration((uint32_t)summary->moving_seconds, time_text,
                                    sizeof(time_text))) {
        snprintf(time_text, sizeof(time_text), "--");
    }
    // "MOVING", not "TIME". The difference is however long the rider stood at
    // traffic lights, and calling it "time" would invite the rider to compare
    // it against the clock and conclude the device lost some.
    Row(s_root, "MOVING", time_text, COLOR_VALUE);

    snprintf(buf, sizeof(buf), "%.1f %s", (double)Settings_SpeedFromKmh(summary->avg_kmh),
             Settings_SpeedUnitLabel());
    Row(s_root, "AVG SPEED", buf, COLOR_VALUE);

    snprintf(buf, sizeof(buf), "%.1f %s", (double)Settings_SpeedFromKmh(summary->max_kmh),
             Settings_SpeedUnitLabel());
    Row(s_root, "MAX SPEED", buf, COLOR_VALUE);

    // Metres, whatever the unit setting says, matching the dashboard's GAIN
    // row. One inconsistent unit beats two screens disagreeing about the same
    // number.
    snprintf(buf, sizeof(buf), "%d m", (int)(summary->ascent_m + 0.5f));
    Row(s_root, "CLIMB", buf, COLOR_VALUE);

    // Zone-coloured, exactly as on the dashboard. A summary that showed the
    // figures in white would throw away the one thing that makes them mean
    // something at a glance.
    const uint8_t rest = Settings_GetHrRestBpm();
    const uint8_t max = Settings_GetHrMaxBpm();
    if (summary->avg_bpm > 0) {
        snprintf(buf, sizeof(buf), "%u bpm", (unsigned)summary->avg_bpm);
        Row(s_root, "AVG HR", buf, ZONE_COLORS[HrZone_Index(summary->avg_bpm, rest, max)]);
        snprintf(buf, sizeof(buf), "%u bpm", (unsigned)summary->max_bpm);
        Row(s_root, "MAX HR", buf, ZONE_COLORS[HrZone_Index(summary->max_bpm, rest, max)]);
    } else {
        Row(s_root, "HEART RATE", "no strap", COLOR_CAPTION);
    }

    lv_obj_t *divider = lv_obj_create(s_root);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(divider, lv_color_hex(COLOR_RULE), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);

    lv_obj_t *file = lv_label_create(s_root);
    if (summary->points > 0 && summary->file[0] != '\0') {
        lv_label_set_text_fmt(file, "%s\n%u points", summary->file, (unsigned)summary->points);
    } else {
        // Says which of the two reasons it was, because they need different
        // fixes: no card is a card problem, no fix is a sky problem.
        lv_label_set_text(file, "Nothing was written to the card.");
    }
    lv_obj_set_style_text_font(file, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(file, lv_color_hex(COLOR_CAPTION), 0);
    lv_label_set_long_mode(file, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(file, LV_PCT(100));

    lv_obj_t *close = lv_btn_create(s_root);
    lv_obj_set_width(close, LV_PCT(100));
    lv_obj_set_height(close, 36);
    lv_obj_set_style_bg_color(close, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_bg_color(close, lv_color_hex(COLOR_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(close, 0, 0);
    lv_obj_add_event_cb(close, OnCloseClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *close_label = lv_label_create(close);
    lv_label_set_text(close_label, LV_SYMBOL_OK "  Done");
    lv_obj_set_style_text_font(close_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(close_label, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_center(close_label);
}
