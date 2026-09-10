#include "Page_Dashboard.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../hal/Battery.h"
#include "../system/DataCenter.h"
#include "../system/PageManager/PageManager.h"
#include "../system/Settings.h"
#include "../system/TimeZone.h"
#include "Page_Map.h"

// Visual design ported from the agents/lvgl-ui-layout-speed-odometer-clock
// branch (c81da8c): dark slate background, one oversized speed readout, and a
// caption+value pair per secondary metric. That branch was a single-threaded
// demo driving LVGL straight from loop() with simulated values; only its
// layout and palette are taken here. The data path stays this branch's
// DataCenter pub/sub with Core-0-safe dirty flags.

namespace {

// ---- Palette (from the ported design) ----
constexpr uint32_t COLOR_BG = 0x101820;      // screen background
constexpr uint32_t COLOR_CAPTION = 0x93A4B8; // small all-caps labels
constexpr uint32_t COLOR_VALUE = 0xFFFFFF;   // primary readouts
constexpr uint32_t COLOR_ACCENT = 0x61DAFB;  // units and incline
constexpr uint32_t COLOR_BADGE_TEXT = 0x081015;

// A turn older than this is treated as gone (phone closed, app backgrounded,
// link dropped without a clean disconnect).
constexpr uint32_t TBT_STALE_MS = 30000;

// Heart-rate zone bands and their badge colours.
constexpr uint8_t ZONE2_LOW = 90;
constexpr uint8_t ZONE3_LOW = 120;
constexpr uint8_t ZONE4_LOW = 150;
constexpr uint32_t COLOR_ZONE_LOW = 0x7EF0A5;
constexpr uint32_t COLOR_ZONE_MID = 0x7BC8FF;
constexpr uint32_t COLOR_ZONE_HIGH = 0xFFD166;

lv_obj_t *s_speed_label = nullptr;
lv_obj_t *s_speed_unit_label = nullptr;
lv_obj_t *s_trip_label = nullptr;
lv_obj_t *s_clock_label = nullptr;
lv_obj_t *s_clock_caption = nullptr;

// Applying a TZ string calls tzset(), which is not free, so only redo it when
// the zone actually changes -- which is almost never on a bike.
const char *s_active_tz = nullptr;
lv_obj_t *s_incline_label = nullptr;
lv_obj_t *s_hr_label = nullptr;
lv_obj_t *s_hr_zone_label = nullptr;
lv_obj_t *s_route_arrow_label = nullptr;
lv_obj_t *s_route_dir_label = nullptr;
lv_obj_t *s_route_dist_label = nullptr;
uint32_t s_tbt_last_ms = 0;
lv_obj_t *s_battery_label = nullptr;
lv_timer_t *s_refresh_timer = nullptr;

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
volatile bool s_imu_dirty = false;
volatile bool s_battery_dirty = false;
volatile bool s_tbt_dirty = false;

// Trip accumulator, only ever touched from the Core-1 refresh timer (and
// Page_Dashboard_ResetTrip(), which the settings page calls from the same core).
double s_trip_km = 0.0;
double s_prev_lat = 0.0;
double s_prev_lon = 0.0;
bool s_has_prev_fix = false;

// Last speed we were handed, kept so a unit change can re-render immediately
// instead of waiting for the next GPS publish.
float s_last_speed_kmh = 0.0f;
bool s_has_speed = false;

// DataCenter callbacks -- may run on Core 0 (whichever core published). Must
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

void OnImuPublished(const char *topic, const void *data, uint32_t size, void *user_arg) {
    (void)topic;
    (void)data;
    (void)size;
    (void)user_arg;
    s_imu_dirty = true;
}

void OnBatteryPublished(const char *topic, const void *data, uint32_t size, void *user_arg) {
    (void)topic;
    (void)data;
    (void)size;
    (void)user_arg;
    s_battery_dirty = true;
}

void OnTbtPublished(const char *topic, const void *data, uint32_t size, void *user_arg) {
    (void)topic;
    (void)data;
    (void)size;
    (void)user_arg;
    s_tbt_dirty = true;
}

// Great-circle distance in metres. The ported demo used
// TinyGPSPlus::distanceBetween(); this branch has no TinyGPS dependency
// (platformio.ini uses the SparkFun u-blox library), so compute it directly.
double DistanceMetres(double lat1, double lon1, double lat2, double lon2) {
    constexpr double kEarthRadiusM = 6371000.0;
    constexpr double kDegToRad = M_PI / 180.0;

    const double dlat = (lat2 - lat1) * kDegToRad;
    const double dlon = (lon2 - lon1) * kDegToRad;
    const double a = sin(dlat / 2.0) * sin(dlat / 2.0) +
                     cos(lat1 * kDegToRad) * cos(lat2 * kDegToRad) * sin(dlon / 2.0) *
                         sin(dlon / 2.0);
    return 2.0 * kEarthRadiusM * atan2(sqrt(a), sqrt(1.0 - a));
}

// A helper so the caption/value pairs below stay one line each at the call site.
lv_obj_t *MakeLabel(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color,
                    lv_align_t align, lv_coord_t x, lv_coord_t y) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_align(label, align, x, y);
    return label;
}

void UpdateHeartRateZone(uint8_t bpm) {
    uint32_t color = COLOR_ZONE_LOW;
    const char *zone_name = "Zone 1";

    if (bpm >= ZONE4_LOW) {
        color = COLOR_ZONE_HIGH;
        zone_name = "Zone 4";
    } else if (bpm >= ZONE3_LOW) {
        color = COLOR_ZONE_MID;
        zone_name = "Zone 3";
    } else if (bpm >= ZONE2_LOW) {
        color = COLOR_ZONE_LOW;
        zone_name = "Zone 2";
    }

    lv_label_set_text(s_hr_zone_label, zone_name);
    lv_obj_set_style_bg_color(s_hr_zone_label, lv_color_hex(color), 0);
}

// LVGL's built-in symbol font has no diagonal or u-turn arrows, so the
// slight/sharp variants collapse onto the plain left/right glyphs and the
// turn type is carried by the street line instead. Proper maneuver icons
// would need a custom font or image assets.
const char *TbtIconSymbol(uint8_t icon_id) {
    switch (icon_id) {
        case TBT_ICON_STRAIGHT:      return LV_SYMBOL_UP;
        case TBT_ICON_TURN_LEFT:
        case TBT_ICON_SLIGHT_LEFT:
        case TBT_ICON_SHARP_LEFT:    return LV_SYMBOL_LEFT;
        case TBT_ICON_TURN_RIGHT:
        case TBT_ICON_SLIGHT_RIGHT:
        case TBT_ICON_SHARP_RIGHT:   return LV_SYMBOL_RIGHT;
        case TBT_ICON_UTURN:
        case TBT_ICON_ROUNDABOUT:    return LV_SYMBOL_REFRESH;
        case TBT_ICON_ARRIVE:        return LV_SYMBOL_OK;
        case TBT_ICON_NONE:
        default:                     return LV_SYMBOL_UP;
    }
}

// Distances follow the same unit setting as speed: showing kilometres to the
// next turn on a device reading mph would be incoherent.
void FormatTbtDistance(uint32_t metres, char *out, size_t out_size) {
    if (Settings_GetSpeedUnit() == SPEED_UNIT_MPH) {
        const float feet = metres * 3.28084f;
        if (feet < 1000.0f) {
            snprintf(out, out_size, "%u ft", (unsigned)(feet + 0.5f));
        } else {
            snprintf(out, out_size, "%.1f mi", metres / 1609.344f);
        }
        return;
    }
    if (metres < 1000) {
        snprintf(out, out_size, "%u m", (unsigned)metres);
    } else {
        snprintf(out, out_size, "%.1f km", metres / 1000.0f);
    }
}

void ClearTbt() {
    lv_label_set_text(s_route_arrow_label, LV_SYMBOL_UP);
    lv_obj_set_style_text_color(s_route_arrow_label, lv_color_hex(0x3A4854), 0);
    lv_label_set_text(s_route_dist_label, "");
    lv_label_set_text(s_route_dir_label, "NO ROUTE");
}

void RenderSpeedAndTrip() {
    if (s_has_speed) {
        lv_label_set_text_fmt(s_speed_label, "%.1f", Settings_SpeedFromKmh(s_last_speed_kmh));
    }
    lv_label_set_text(s_speed_unit_label, Settings_SpeedUnitLabel());
    lv_label_set_text_fmt(s_trip_label, "%.2f %s", Settings_DistanceFromKm((float)s_trip_km),
                          Settings_DistanceUnitLabel());
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
            s_last_speed_kmh = gps.speed * 3.6f;
            s_has_speed = true;

            // The receiver's own validity flag, not a guess from the
            // coordinates: before a fix, lat/lon are legitimately 0,0. The
            // 1km/tick ceiling still drops the single bogus jump a cold fix
            // can produce as it settles.
            if (gps.fix_valid) {
                if (s_has_prev_fix) {
                    const double step_m = DistanceMetres(s_prev_lat, s_prev_lon, gps.lat, gps.lon);
                    if (step_m < 1000.0) {
                        s_trip_km += step_m / 1000.0;
                    }
                }
                s_prev_lat = gps.lat;
                s_prev_lon = gps.lon;
                s_has_prev_fix = true;
            }
            RenderSpeedAndTrip();

            // Local time, derived from the fix itself: the position picks the
            // timezone and newlib applies its DST rule. No setting, no
            // network. Needs a valid fix as well as valid time -- without a
            // position there is no zone to resolve.
            if (gps.time_valid && gps.fix_valid) {
                bool approximate = false;
                const char *tz = TimeZone_PosixFor(gps.lat, gps.lon, &approximate);

                if (s_active_tz == nullptr || strcmp(s_active_tz, tz) != 0) {
                    setenv("TZ", tz, 1);
                    tzset();
                    s_active_tz = tz;
                }

                const time_t epoch = (time_t)TimeZone_UtcToEpoch(
                    gps.year, gps.month, gps.day, gps.hour, gps.minute, gps.second);
                struct tm local;
                localtime_r(&epoch, &local);

                lv_label_set_text_fmt(s_clock_label, "%02d:%02d:%02d", local.tm_hour,
                                      local.tm_min, local.tm_sec);

                // The caption carries the zone abbreviation newlib resolved
                // (EST, EDT, CST...), so the displayed hour is attributable
                // rather than just asserted. A guessed zone says so.
                char zone[8] = {0};
                strftime(zone, sizeof(zone), "%Z", &local);
                lv_label_set_text_fmt(s_clock_caption, approximate ? "TIME ~%s" : "TIME %s", zone);
            } else if (gps.time_valid) {
                // Time but no fix: UTC is all that can honestly be shown.
                lv_label_set_text_fmt(s_clock_label, "%02u:%02u:%02u", gps.hour, gps.minute,
                                      gps.second);
                lv_label_set_text(s_clock_caption, "TIME UTC");
            }
        }
    }

    if (s_hr_dirty) {
        s_hr_dirty = false;
        HeartRate_t hr;
        if (DataCenter_Pull(TOPIC_HEART_RATE, &hr, sizeof(hr))) {
            lv_label_set_text_fmt(s_hr_label, "%d", hr.bpm);
            UpdateHeartRateZone(hr.bpm);
        }
    }

    if (s_battery_dirty) {
        s_battery_dirty = false;
        Battery_t battery;
        if (DataCenter_Pull(TOPIC_BATTERY, &battery, sizeof(battery))) {
            // Icon steps with the charge so the corner reads at a glance
            // without parsing the number.
            const char *icon = LV_SYMBOL_BATTERY_EMPTY;
            if (battery.on_usb) {
                icon = LV_SYMBOL_CHARGE;
            } else if (battery.percent >= 87) {
                icon = LV_SYMBOL_BATTERY_FULL;
            } else if (battery.percent >= 62) {
                icon = LV_SYMBOL_BATTERY_3;
            } else if (battery.percent >= 37) {
                icon = LV_SYMBOL_BATTERY_2;
            } else if (battery.percent >= 12) {
                icon = LV_SYMBOL_BATTERY_1;
            }
            lv_label_set_text_fmt(s_battery_label, "%s %d%%", icon, battery.percent);
            // Red below the curve's low-battery point, so it stands out
            // against the otherwise uniform caption grey.
            lv_obj_set_style_text_color(
                s_battery_label,
                lv_color_hex((!battery.on_usb && battery.percent <= 10) ? 0xFF6B6B : COLOR_CAPTION),
                0);
        }
    }

    if (s_tbt_dirty) {
        s_tbt_dirty = false;
        TBT_Directive_t tbt;
        if (DataCenter_Pull(TOPIC_NAV_TBT, &tbt, sizeof(tbt))) {
            if (tbt.icon_id == TBT_ICON_NONE) {
                ClearTbt();
                s_tbt_last_ms = 0;
            } else {
                char dist[16];
                FormatTbtDistance(tbt.distance_m, dist, sizeof(dist));
                lv_label_set_text(s_route_arrow_label, TbtIconSymbol(tbt.icon_id));
                lv_obj_set_style_text_color(s_route_arrow_label, lv_color_hex(COLOR_ACCENT), 0);
                lv_label_set_text(s_route_dist_label, dist);
                lv_label_set_text(s_route_dir_label,
                                  tbt.street_name[0] != '\0' ? tbt.street_name : "AHEAD");
                s_tbt_last_ms = lv_tick_get();
            }
        }
    }

    // Drop a stale turn rather than leaving the rider following an
    // instruction the phone stopped confirming.
    if (s_tbt_last_ms != 0 && lv_tick_elaps(s_tbt_last_ms) > TBT_STALE_MS) {
        ClearTbt();
        s_tbt_last_ms = 0;
    }

    if (s_imu_dirty) {
        s_imu_dirty = false;
        IMU_Data_t imu;
        if (DataCenter_Pull(TOPIC_IMU_DATA, &imu, sizeof(imu))) {
            // Grade as a percentage of rise over run, from the IMU's pitch.
            const float grade = tanf(imu.pitch * (float)M_PI / 180.0f) * 100.0f;
            lv_label_set_text_fmt(s_incline_label, "%+.1f%%", grade);
        }
    }
}

void OnMapClicked(lv_event_t *e) {
    PageDashboard *self = (PageDashboard *)lv_event_get_user_data(e);
    if (self != nullptr && self->_Manager != nullptr) {
        self->_Manager->Push(PAGE_NAME_MAP);
    }
}

void OnSettingsClicked(lv_event_t *e) {
    PageDashboard *self = (PageDashboard *)lv_event_get_user_data(e);
    if (self != nullptr && self->_Manager != nullptr) {
        self->_Manager->Push(PAGE_NAME_SETTINGS);
    }
}

// One Account per subscription. File-scope rather than members because the
// DataCenter callbacks above are plain functions and there is only ever one
// dashboard instance.
Account s_gps_account("Page_Dashboard/GPS", OnGpsPublished);
Account s_hr_account("Page_Dashboard/HeartRate", OnHeartRatePublished);
Account s_imu_account("Page_Dashboard/IMU", OnImuPublished);
Account s_battery_account("Page_Dashboard/Battery", OnBatteryPublished);
Account s_tbt_account("Page_Dashboard/TBT", OnTbtPublished);

} // namespace

void Page_Dashboard_ResetTrip() {
    s_trip_km = 0.0;
    s_has_prev_fix = false;
    if (s_trip_label != nullptr) {
        RenderSpeedAndTrip();
    }
}

PageDashboard::PageDashboard() {}

void PageDashboard::onViewLoad() {
    lv_obj_t *parent = _root;
    lv_obj_set_style_bg_color(parent, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    MakeLabel(parent, "PEGASUS", &lv_font_montserrat_14, COLOR_CAPTION, LV_ALIGN_TOP_MID, 0, 14);

    // ---- Settings button (status-bar corner) ----
    // The project's first interactive widget: everything else on this page is
    // a passive readout.
    lv_obj_t *settings_btn = lv_btn_create(parent);
    lv_obj_set_size(settings_btn, 40, 32);
    lv_obj_align(settings_btn, LV_ALIGN_TOP_LEFT, 6, 6);
    lv_obj_set_style_bg_color(settings_btn, lv_color_hex(0x1D2A36), 0);
    lv_obj_set_style_bg_color(settings_btn, lv_color_hex(COLOR_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_radius(settings_btn, 8, 0);
    lv_obj_set_style_shadow_width(settings_btn, 0, 0);
    lv_obj_add_event_cb(settings_btn, OnSettingsClicked, LV_EVENT_CLICKED, this);

    lv_obj_t *gear = lv_label_create(settings_btn);
    lv_label_set_text(gear, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(gear, lv_color_hex(COLOR_VALUE), 0);
    lv_obj_center(gear);

    // ---- Map button ----
    // Only in GPX mode: in TBT mode there is no trail to draw, and the ROUTE
    // panel below already carries the turn. Offering a button to an empty map
    // would be a dead end.
    if (Settings_GetNavMode() == NAV_MODE_GPX) {
        lv_obj_t *map_btn = lv_btn_create(parent);
        lv_obj_set_size(map_btn, 40, 32);
        lv_obj_align(map_btn, LV_ALIGN_TOP_LEFT, 50, 6);
        lv_obj_set_style_bg_color(map_btn, lv_color_hex(0x1D2A36), 0);
        lv_obj_set_style_bg_color(map_btn, lv_color_hex(COLOR_ACCENT), LV_STATE_PRESSED);
        lv_obj_set_style_radius(map_btn, 8, 0);
        lv_obj_set_style_shadow_width(map_btn, 0, 0);
        lv_obj_add_event_cb(map_btn, OnMapClicked, LV_EVENT_CLICKED, this);

        lv_obj_t *map_icon = lv_label_create(map_btn);
        lv_label_set_text(map_icon, LV_SYMBOL_GPS);
        lv_obj_set_style_text_color(map_icon, lv_color_hex(COLOR_VALUE), 0);
        lv_obj_center(map_icon);
    }

    // ---- Battery (status-bar corner) ----
    // The device's own battery, published to TOPIC_BATTERY by the Core 0
    // monitor in hal/Battery.cpp -- not to be confused with
    // HeartRate_t.battery, which is the HR strap's.
    s_battery_label = MakeLabel(parent, LV_SYMBOL_BATTERY_FULL " --%", &lv_font_montserrat_12,
                                COLOR_CAPTION, LV_ALIGN_TOP_RIGHT, -14, 16);

    // ---- Speed: the one value readable at a glance while riding ----
    s_speed_label = MakeLabel(parent, "--", &lv_font_montserrat_48, COLOR_VALUE, LV_ALIGN_TOP_MID,
                              0, 38);
    s_speed_unit_label = MakeLabel(parent, Settings_SpeedUnitLabel(), &lv_font_montserrat_14,
                                   COLOR_ACCENT, LV_ALIGN_TOP_MID, 0, 92);

    // ---- Trip ----
    MakeLabel(parent, "TRIP", &lv_font_montserrat_12, COLOR_CAPTION, LV_ALIGN_TOP_LEFT, 18, 118);
    s_trip_label = MakeLabel(parent, "0.00 km", &lv_font_montserrat_24, COLOR_VALUE,
                             LV_ALIGN_TOP_LEFT, 18, 138);

    // ---- Clock ----
    // Fed by UBX NAV-PVT, which carries UTC alongside the position, so no RTC
    // is needed -- and the position also chooses the timezone, so the clock
    // reads local time with no setting to get wrong. Stays "--:--:--" until
    // the receiver reports the time fully resolved.
    s_clock_caption = MakeLabel(parent, "TIME", &lv_font_montserrat_10, COLOR_CAPTION,
                                LV_ALIGN_TOP_LEFT, 18, 196);
    s_clock_label = MakeLabel(parent, "--:--:--", &lv_font_montserrat_18, COLOR_VALUE,
                              LV_ALIGN_TOP_LEFT, 18, 212);

    // ---- Incline ----
    MakeLabel(parent, "INCLINE", &lv_font_montserrat_10, COLOR_CAPTION, LV_ALIGN_TOP_MID, 0, 196);
    s_incline_label = MakeLabel(parent, "--%", &lv_font_montserrat_18, COLOR_ACCENT,
                                LV_ALIGN_TOP_MID, 0, 212);

    // ---- Heart rate ----
    MakeLabel(parent, "HEART RATE", &lv_font_montserrat_10, COLOR_CAPTION, LV_ALIGN_TOP_RIGHT, -18,
              196);
    s_hr_label = MakeLabel(parent, "--", &lv_font_montserrat_18, COLOR_VALUE, LV_ALIGN_TOP_RIGHT,
                           -38, 212);
    MakeLabel(parent, "bpm", &lv_font_montserrat_10, COLOR_VALUE, LV_ALIGN_TOP_RIGHT, -12, 220);

    s_hr_zone_label = lv_label_create(parent);
    lv_label_set_text(s_hr_zone_label, "--");
    lv_obj_set_style_text_font(s_hr_zone_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hr_zone_label, lv_color_hex(COLOR_BADGE_TEXT), 0);
    lv_obj_set_style_bg_color(s_hr_zone_label, lv_color_hex(COLOR_ZONE_LOW), 0);
    lv_obj_set_style_bg_opa(s_hr_zone_label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(s_hr_zone_label, 8, 0);
    lv_obj_set_style_pad_ver(s_hr_zone_label, 2, 0);
    lv_obj_set_style_radius(s_hr_zone_label, 8, 0);
    lv_obj_align(s_hr_zone_label, LV_ALIGN_TOP_RIGHT, -18, 238);

    // ---- Route / turn-by-turn ----
    // Fed by TOPIC_NAV_TBT from src/navigation/BLE_TBT_Receiver.cpp, which the
    // phone writes into over BLE (CLAUDE.md §5).
    MakeLabel(parent, "ROUTE", &lv_font_montserrat_10, COLOR_CAPTION, LV_ALIGN_TOP_RIGHT, -18, 256);
    s_route_arrow_label = MakeLabel(parent, LV_SYMBOL_UP, &lv_font_montserrat_28, COLOR_ACCENT,
                                    LV_ALIGN_TOP_RIGHT, -18, 268);
    s_route_dist_label = MakeLabel(parent, "", &lv_font_montserrat_18, COLOR_VALUE,
                                   LV_ALIGN_TOP_RIGHT, -48, 276);

    // Street names run long; clip with an ellipsis instead of letting the text
    // run left across the heart-rate column.
    s_route_dir_label = lv_label_create(parent);
    lv_obj_set_width(s_route_dir_label, 132);
    lv_label_set_long_mode(s_route_dir_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_route_dir_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(s_route_dir_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_route_dir_label, lv_color_hex(COLOR_VALUE), 0);
    lv_obj_align(s_route_dir_label, LV_ALIGN_TOP_RIGHT, -18, 300);

    ClearTbt();

    RenderSpeedAndTrip();

    DataCenter_Subscribe(TOPIC_GPS_INFO, &s_gps_account);
    DataCenter_Subscribe(TOPIC_HEART_RATE, &s_hr_account);
    DataCenter_Subscribe(TOPIC_IMU_DATA, &s_imu_account);
    DataCenter_Subscribe(TOPIC_BATTERY, &s_battery_account);
    DataCenter_Subscribe(TOPIC_NAV_TBT, &s_tbt_account);

    s_refresh_timer = lv_timer_create(RefreshTimerCallback, 100, nullptr);
}

void PageDashboard::onViewUnload() {
    // The timer outlives the widgets unless it is torn down here, and would
    // then write to freed lv_obj pointers on its next tick.
    if (s_refresh_timer != nullptr) {
        lv_timer_del(s_refresh_timer);
        s_refresh_timer = nullptr;
    }

    DataCenter_Unsubscribe(TOPIC_GPS_INFO, &s_gps_account);
    DataCenter_Unsubscribe(TOPIC_HEART_RATE, &s_hr_account);
    DataCenter_Unsubscribe(TOPIC_IMU_DATA, &s_imu_account);
    DataCenter_Unsubscribe(TOPIC_BATTERY, &s_battery_account);
    DataCenter_Unsubscribe(TOPIC_NAV_TBT, &s_tbt_account);

    s_speed_label = nullptr;
    s_speed_unit_label = nullptr;
    s_trip_label = nullptr;
    s_clock_label = nullptr;
    s_clock_caption = nullptr;
    s_active_tz = nullptr;
    s_incline_label = nullptr;
    s_hr_label = nullptr;
    s_hr_zone_label = nullptr;
    s_route_arrow_label = nullptr;
    s_route_dir_label = nullptr;
    s_route_dist_label = nullptr;
    s_battery_label = nullptr;
}
