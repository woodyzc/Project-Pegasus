#include "Page_Dashboard.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../hal/Battery.h"
#include "../system/DataCenter.h"
#include "../system/HrZone.h"
#include "../system/PageManager/PageManager.h"
#include "../system/Settings.h"
#include "../system/Trip.h"
#include "../navigation/GpxTrack.h"
#include "../navigation/TbtParse.h"
#include "../system/TimeZone.h"
#include "MapView.h"
#include "RoadView.h"
#include "Page_Map.h"
#include "TbtIcons.h"

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

// Turn-by-turn computed on board from the cached route rather than received
// live from the phone. Colour rather than a word or an icon: the navigation
// tile is already the densest thing on the panel, and at a junction the rider
// has no attention to spare for reading a mode label. Amber says "still
// navigating, but on my own" at a glance, and red says the fix is nowhere near
// the route so the countdown cannot be trusted.
constexpr uint32_t COLOR_NAV_ONBOARD = 0xFFC857;
constexpr uint32_t COLOR_NAV_OFF_ROUTE = 0xFF6B6B;
constexpr uint32_t COLOR_BADGE_TEXT = 0x081015;
constexpr uint32_t COLOR_CELL_BG = 0x141E27;
constexpr uint32_t COLOR_CELL_BORDER = 0x24313D;
// Filled behind the incline value on a real climb: the grade matters most
// when it is large, and colour carries that faster than digits do.
constexpr uint32_t COLOR_CLIMB_FILL = 0x3A2E12;

// A turn older than this is treated as gone (phone closed, app backgrounded,
// link dropped without a clean disconnect).
constexpr uint32_t TBT_STALE_MS = 30000;

// A heart-rate source notifies about once a second, so this much silence means
// the link is gone or the peer stopped sending -- not a gap between beats.
//
// Without it a bpm stayed on screen indefinitely: stop the watch broadcasting
// and the last reading sat there looking live, which is worse than "--"
// because a rider has no way to tell it is minutes old.
constexpr uint32_t HR_STALE_MS = 5000;

// One colour per training zone: blue, green, yellow, red, purple. Saturated
// rather than the pastels the rest of the panel uses, because this bar is read
// in sunlight at a glance and a washed-out band is the one thing it cannot
// afford. The boundaries themselves live in HrZone.h, scaled to the rider's
// own resting and maximum rate.
const uint32_t ZONE_COLORS[HR_ZONE_COUNT] = {
    0x0A84FF, // 1  low intensity    blue
    0x30D158, // 2  weight control   green
    0xFFD60A, // 3  aerobic          yellow
    0xFF3B30, // 4  anaerobic        red
    0xBF5AF2, // 5  maximum          purple
};

lv_obj_t *s_speed_label = nullptr;
lv_obj_t *s_speed_unit_label = nullptr;
lv_obj_t *s_trip_label = nullptr;
lv_obj_t *s_trip_unit_label = nullptr;
lv_obj_t *s_clock_label = nullptr;
lv_obj_t *s_clock_caption = nullptr;

// Applying a TZ string calls tzset(), which is not free, so only redo it when
// the zone actually changes -- which is almost never on a bike.
const char *s_active_tz = nullptr;
lv_obj_t *s_incline_label = nullptr;
lv_obj_t *s_hr_label = nullptr;
lv_obj_t *s_incline_cell = nullptr;
lv_obj_t *s_zone_segments[HR_ZONE_COUNT] = {nullptr, nullptr, nullptr, nullptr, nullptr};

// A triangle riding above the bar, pointing down at the rider's position. The
// lit segment says which zone; this says where inside it, which is the
// difference between holding the bottom of zone 4 and being about to fall out
// of the top.
//
// LVGL 8 has no triangle to draw with: the symbol font carries chevrons and
// arrows but no solid wedge, and a rotated object only works for images. This
// was briefly an lv_canvas polygon, which made the board reboot -- a canvas in
// LV_IMG_CF_TRUE_COLOR_ALPHA needs LV_COLOR_SCREEN_TRANSP, and with that off
// (it is, and it is a whole-display rendering mode, not something to turn on
// for one 15px marker) the software renderer's alpha-blend paths are compiled
// out from under it.
//
// So: four stacked rows, each narrower than the last, in a transparent
// container that moves as one. Plain lv_obj rectangles, the same thing every
// other widget on this page is made of, with no rendering mode behind them
// that can be absent.
constexpr lv_coord_t ZONE_MARKER_W = 15;
constexpr lv_coord_t ZONE_MARKER_H = 8;
constexpr lv_coord_t ZONE_MARKER_ROWS = 4;
constexpr lv_coord_t ZONE_MARKER_ROW_H = ZONE_MARKER_H / ZONE_MARKER_ROWS; // 2
constexpr lv_coord_t ZONE_MARKER_STEP = 4; // width lost per row, so 15/11/7/3
lv_obj_t *s_zone_marker = nullptr;

// Bar geometry, recorded when the bar is built so the marker can be placed
// without the layout constants leaking out of onViewLoad.
lv_coord_t s_zone_bar_w = 0;

// The navigation slot holds one of two things depending on the chosen mode:
// a turn card in TBT, or a live breadcrumb map in GPX. Only one is created,
// so the other costs nothing.
lv_obj_t *s_nav_cell = nullptr;
MapView_t s_map_view;
bool s_nav_is_map = false;

// Storage for the inline map. lv_line keeps a pointer to the point array
// rather than copying it, so these must be file-scope.
constexpr size_t INLINE_MAP_POINTS = 256;
lv_point_t s_map_points[INLINE_MAP_POINTS];
MapPoint_t s_map_projected[INLINE_MAP_POINTS];
// An lv_img, not a label: the maneuver arrows are generated bitmaps now
// (TbtIcons.h). Kept under the old name because every reference to it means
// the same thing -- the thing in the corner that shows which way to go.
lv_obj_t *s_route_arrow_label = nullptr;
lv_obj_t *s_route_dir_label = nullptr;
lv_obj_t *s_route_dist_label = nullptr;
lv_obj_t *s_route_dist_unit = nullptr;
uint32_t s_tbt_last_ms = 0;
uint32_t s_hr_last_ms = 0;
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

// Cell internals, shared by the caption, the unit and the value so the three
// line up by construction rather than by three call sites agreeing.
// Both margins are as tight as they are because the value font grew to fill
// what they gave up: an 11px caption at y=2 and a 44px value 1px off the
// bottom of a 60px cell leaves the two boxes 2px apart.
constexpr lv_coord_t CELL_PAD = 6;         // left and right inset
constexpr lv_coord_t CELL_CAPTION_Y = 2;   // caption and unit baseline row
constexpr lv_coord_t CELL_VALUE_Y = -1;    // value, up from the cell's bottom

// One bordered cell: caption at the top, value at the bottom. Cells bound
// their contents, so a long value cannot drift into a neighbour -- which is
// exactly how the clock ended up on top of the incline figure when these were
// free-floating labels.
lv_obj_t *MakeCell(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                   const char *caption) {
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_set_size(cell, w, h);
    lv_obj_set_pos(cell, x, y);
    lv_obj_set_style_bg_color(cell, lv_color_hex(COLOR_CELL_BG), 0);
    // No border and no rounding: widgets tile the panel and are divided by a
    // single shared hairline between neighbours (MakeSeparator below). A
    // border per cell draws two lines down every internal join and four more
    // around the outside, which on a 240px panel is a lot of the screen spent
    // on frames.
    lv_obj_set_style_border_width(cell, 0, 0);
    lv_obj_set_style_radius(cell, 0, 0);
    lv_obj_set_style_pad_all(cell, 0, 0);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(cell);
    lv_label_set_text(label, caption);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_CAPTION), 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, CELL_PAD, CELL_CAPTION_Y);
    return cell;
}

// One hairline between two tiles. Only ever drawn on an internal join --
// never around the outside, so widgets run into the screen edge cleanly.
lv_obj_t *MakeSeparator(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                        lv_coord_t h) {
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_size(line, w, h);
    lv_obj_set_pos(line, x, y);
    lv_obj_set_style_bg_color(line, lv_color_hex(COLOR_CELL_BORDER), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    return line;
}

// The number itself, as large as a 60px cell permits. 40 is the ceiling, and
// both dimensions of the cell set it. Vertically, line_height 44 against an
// 11px caption leaves 2px; 42 makes them touch. Horizontally, measured against
// the real glyph widths rather than a digit count, since "1" and "." are much
// narrower than "8" -- inside 108px of usable width:
//
//     SPEED    "105.3"    96px
//     TRIP     "99.99"   108px   <- the binding case, exactly at the limit
//     INCLINE  "-12.5"    85px
//     HR       "188"      66px
//
// The trip is why RenderSpeedAndTrip drops to one decimal at 100km: "123.45"
// wants 119px and would run out of the cell.
//
// Both this and MakeUnit place their label with lv_obj_align rather than
// lv_obj_align_to, and that difference is load-bearing. align_to positions
// once, against the other label's size at that instant; an alignment set by
// lv_obj_align is stored on the object and re-applied every time the label
// resizes. These labels change width constantly -- 9.5 to 10.5, "km/h" to
// "mph" -- so a one-shot placement silently goes stale.
lv_obj_t *MakeValue(lv_obj_t *cell, const char *text, uint32_t color) {
    lv_obj_t *label = lv_label_create(cell);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, CELL_PAD, CELL_VALUE_Y);
    return label;
}

// The unit sits on the caption row, at the opposite end of the cell: "SPEED"
// on the left, "km/h" on the right, both in the same small grey. It used to
// hang off the right of the value, which cost the number the width it needed
// and pinned the label to a moving target.
lv_obj_t *MakeUnit(lv_obj_t *cell, const char *text) {
    lv_obj_t *label = lv_label_create(cell);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_CAPTION), 0);
    lv_obj_align(label, LV_ALIGN_TOP_RIGHT, -CELL_PAD, CELL_CAPTION_Y);
    return label;
}

// Dims every band, hides the marker and returns the reading to plain white:
// no reading means no zone, and a coloured "--" would still be asserting
// something about the rider.
void ClearHeartRateZone() {
    for (int i = 0; i < HR_ZONE_COUNT; i++) {
        if (s_zone_segments[i] != nullptr) {
            lv_obj_set_style_bg_opa(s_zone_segments[i], LV_OPA_40, 0);
        }
    }
    if (s_zone_marker != nullptr) {
        lv_obj_add_flag(s_zone_marker, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_hr_label != nullptr) {
        lv_obj_set_style_text_color(s_hr_label, lv_color_hex(COLOR_VALUE), 0);
    }
}

void UpdateHeartRateZone(uint8_t bpm) {
    const uint8_t rest = Settings_GetHrRestBpm();
    const uint8_t max = Settings_GetHrMaxBpm();
    const int zone = HrZone_Index(bpm, rest, max);

    // The number takes its zone's colour too. The bar is 8px at the very
    // bottom of the panel; the bpm figure is the thing already being looked at,
    // so colouring it means the zone registers without the eye travelling.
    if (s_hr_label != nullptr) {
        lv_obj_set_style_text_color(s_hr_label, lv_color_hex(ZONE_COLORS[zone]), 0);
    }

    // The lit segment is the readout: colour and position carry the zone at a
    // glance on a bouncing bike, where the words "Zone 3" do not.
    for (int i = 0; i < HR_ZONE_COUNT; i++) {
        if (s_zone_segments[i] != nullptr) {
            lv_obj_set_style_bg_opa(s_zone_segments[i], (i == zone) ? LV_OPA_COVER : LV_OPA_40, 0);
        }
    }

    if (s_zone_marker != nullptr) {
        // EqualWidthFraction, not Fraction: the segments are all one width now,
        // so a position measured in reserve would drift out of the lit segment.
        const double position = HrZone_EqualWidthFraction(bpm, rest, max);

        // Place the APEX on the position and hang the marker either side of
        // it, rather than sliding the whole marker across a shortened travel.
        // The shortened travel is the tempting version and it is wrong: it
        // compresses the marker's range to 225px while the segments still
        // divide 240, so by zone 4 the triangle points a segment to the left
        // of the one that is lit.
        //
        // Clamping the marker instead of the apex confines the error to the
        // two ends, where the triangle would otherwise hang off the panel:
        // at rest and at maximum the apex sits half a triangle inside the
        // edge, and both are still well within their own segment.
        const lv_coord_t apex = (lv_coord_t)lround(position * (double)s_zone_bar_w);
        lv_coord_t x = apex - ZONE_MARKER_W / 2;
        if (x < 0) {
            x = 0;
        }
        if (x > s_zone_bar_w - ZONE_MARKER_W) {
            x = s_zone_bar_w - ZONE_MARKER_W;
        }
        lv_obj_set_x(s_zone_marker, x);
        lv_obj_clear_flag(s_zone_marker, LV_OBJ_FLAG_HIDDEN);
    }
}

// Distances follow the same unit setting as speed: showing kilometres to the
// next turn on a device reading mph would be incoherent.
// Splits the distance into the number and its unit, which are drawn at very
// different sizes.
//
// They used to be one string at 48pt, and that is what kept the turn arrow
// small: "200 m" alone needs 155 of the panel's 240px, leaving under 70px
// beside it. Separating them lets the number keep the biggest face LVGL
// offers while the unit drops to a caption, and the width that buys goes to
// the arrow.
void FormatTbtDistance(uint32_t metres, char *value, size_t value_size, char *unit,
                       size_t unit_size) {
    // Maps sometimes names the turn without saying how far away it is. The
    // arrow and the street are still worth showing; a fabricated distance is
    // not, so the field says plainly that it does not know.
    if (metres == TBT_DISTANCE_UNKNOWN) {
        snprintf(value, value_size, "--");
        snprintf(unit, unit_size, "");
        return;
    }
    if (Settings_GetSpeedUnit() == SPEED_UNIT_MPH) {
        const float feet = metres * 3.28084f;
        if (feet < 1000.0f) {
            snprintf(value, value_size, "%u", (unsigned)(feet + 0.5f));
            snprintf(unit, unit_size, "ft");
        } else {
            snprintf(value, value_size, "%.1f", metres / 1609.344f);
            snprintf(unit, unit_size, "mi");
        }
        return;
    }
    if (metres < 1000) {
        snprintf(value, value_size, "%u", (unsigned)metres);
        snprintf(unit, unit_size, "m");
    } else {
        snprintf(value, value_size, "%.1f", metres / 1000.0f);
        snprintf(unit, unit_size, "km");
    }
}

void ClearTbt() {
    // In GPX mode the slot holds a map and these labels do not exist.
    if (s_route_arrow_label == nullptr) {
        return;
    }
    lv_img_set_src(s_route_arrow_label, TbtIcon(TBT_ICON_NONE));
    lv_obj_set_style_img_recolor(s_route_arrow_label, lv_color_hex(0x3A4854), 0);
    lv_label_set_text(s_route_dist_label, "");
    lv_label_set_text(s_route_dist_unit, "");
    lv_label_set_text(s_route_dir_label, "NO ROUTE");
}

void RenderSpeedAndTrip() {
    if (s_has_speed) {
        lv_label_set_text_fmt(s_speed_label, "%.1f", Settings_SpeedFromKmh(s_last_speed_kmh));
    }
    lv_label_set_text(s_speed_unit_label, Settings_SpeedUnitLabel());
    // Two decimals until three digits are needed, then one. At 40px "123.45"
    // is 119px in a cell that can show 108, so the choice is between dropping
    // a decimal and dropping a digit -- and 10m resolution stops being worth
    // anything a long way before 100km. Under 100 nothing changes.
    const float trip = Settings_DistanceFromKm((float)Trip_Km());
    lv_label_set_text_fmt(s_trip_label, (trip >= 100.0f) ? "%.1f" : "%.2f", trip);
    if (s_trip_unit_label != nullptr) {
        lv_label_set_text(s_trip_unit_label, Settings_DistanceUnitLabel());
    }
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

            // Distance is no longer accumulated here. Trip owns it and reads
            // the same GPS topic directly, so the odometer keeps counting
            // while the rider is looking at the map or the settings page --
            // this callback only runs when the dashboard is the loaded page.
            RenderSpeedAndTrip();

            // The inline map follows the rider on the same publish that moves
            // the speed readout.
            if (s_nav_is_map) {
                MapView_SetPosition(&s_map_view, &gps);
                RoadView_Refresh();
            }

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

                lv_label_set_text_fmt(s_clock_label, "%02d:%02d", local.tm_hour, local.tm_min);

                // The caption carries the zone abbreviation newlib resolved
                // (EST, EDT, CST...), so the displayed hour is attributable
                // rather than just asserted. A guessed zone says so.
                char zone[8] = {0};
                strftime(zone, sizeof(zone), "%Z", &local);
                lv_label_set_text_fmt(s_clock_caption, approximate ? "~%s" : "%s", zone);
            } else if (gps.time_valid) {
                // Time but no fix: UTC is all that can honestly be shown.
                lv_label_set_text_fmt(s_clock_label, "%02u:%02u", gps.hour, gps.minute);
                lv_label_set_text(s_clock_caption, "UTC");
            }
        }
    }

    if (s_hr_dirty) {
        s_hr_dirty = false;
        HeartRate_t hr;
        if (DataCenter_Pull(TOPIC_HEART_RATE, &hr, sizeof(hr))) {
            lv_label_set_text_fmt(s_hr_label, "%d", hr.bpm);
            UpdateHeartRateZone(hr.bpm);
            s_hr_last_ms = lv_tick_get();
        }
    }

    // Same reasoning as the turn above: a reading nobody is confirming any
    // more gets dropped rather than left looking current.
    if (s_hr_last_ms != 0 && lv_tick_elaps(s_hr_last_ms) > HR_STALE_MS) {
        lv_label_set_text(s_hr_label, "--");
        ClearHeartRateZone();
        s_hr_last_ms = 0;
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

    if (s_tbt_dirty && !s_nav_is_map) {
        s_tbt_dirty = false;
        TBT_Directive_t tbt;
        if (DataCenter_Pull(TOPIC_NAV_TBT, &tbt, sizeof(tbt))) {
            if (tbt.icon_id == TBT_ICON_NONE) {
                ClearTbt();
                s_tbt_last_ms = 0;
            } else {
                char dist[16];
                char dist_unit[8];
                FormatTbtDistance(tbt.distance_m, dist, sizeof(dist), dist_unit,
                                  sizeof(dist_unit));
                // The arrow carries the source. See the colour constants.
                uint32_t arrow_colour = COLOR_ACCENT;
                if (tbt.off_route) {
                    arrow_colour = COLOR_NAV_OFF_ROUTE;
                } else if (tbt.source == TBT_SOURCE_ONBOARD) {
                    arrow_colour = COLOR_NAV_ONBOARD;
                }
                lv_img_set_src(s_route_arrow_label, TbtIcon(tbt.icon_id));
                lv_obj_set_style_img_recolor(s_route_arrow_label, lv_color_hex(arrow_colour), 0);
                lv_label_set_text(s_route_dist_label, dist);
                lv_label_set_text(s_route_dist_unit, dist_unit);
                lv_label_set_text(s_route_dir_label,
                                  tbt.street_name[0] != '\0' ? tbt.street_name : "AHEAD");
                s_tbt_last_ms = lv_tick_get();
            }
        }
    }

    // Drop a stale turn rather than leaving the rider following an
    // instruction the phone stopped confirming.
    if (!s_nav_is_map && s_tbt_last_ms != 0 && lv_tick_elaps(s_tbt_last_ms) > TBT_STALE_MS) {
        ClearTbt();
        s_tbt_last_ms = 0;
    }

    if (s_imu_dirty) {
        s_imu_dirty = false;
        IMU_Data_t imu;
        if (DataCenter_Pull(TOPIC_IMU_DATA, &imu, sizeof(imu))) {
            // Grade as a percentage of rise over run, from the IMU's pitch.
            const float grade = tanf(imu.pitch * (float)M_PI / 180.0f) * 100.0f;
            lv_label_set_text_fmt(s_incline_label, "%+.1f", grade);
            lv_obj_set_style_bg_color(
                s_incline_cell, lv_color_hex(grade >= 3.0f ? COLOR_CLIMB_FILL : COLOR_CELL_BG), 0);
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
    Trip_Reset();
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

    // ---- Geometry ----
    // Every position below derives from these, so the layout cannot drift out
    // of agreement with itself. The panel is 240x320 and the widgets now use
    // all of it: the previous layout inset everything by 18px a side, which
    // spent 15% of a 240px-wide screen on nothing.
    // Widgets tile the panel edge to edge with no margin and no gap; a single
    // hairline divides neighbours, and none is drawn where a widget meets the
    // screen edge. Every position derives from these so the tiling stays
    // exact -- a one-pixel disagreement shows as a seam or an overlap.
    const lv_coord_t SCREEN_W = 240;
    const lv_coord_t SCREEN_H = 320;
    const lv_coord_t PAD = 6; // inside a tile, between its edge and its text

    // The vertical budget is now measured from the bottom up, because the two
    // regions down there have hard minimums and navigation does not. A metric
    // cell cannot go below 60px without the 40px value colliding with its
    // caption, and the zone block needs 16 to give the marker a row of its own
    // above the colours. Navigation takes what is left, which is 184 -- it was
    // a flat 60% of the panel (192) until the marker needed those 8px.
    const lv_coord_t ZONE_BAR_H = 8;                           // the colour bands
    const lv_coord_t ZONE_MARK_H = 8;                          // the triangle above them
    const lv_coord_t CELL_H = 60;
    const lv_coord_t CELL_W = SCREEN_W / 2;                    // 120
    const lv_coord_t COL1 = 0;
    const lv_coord_t COL2 = CELL_W;                            // 120

    const lv_coord_t ZONE_BAR_Y = SCREEN_H - ZONE_BAR_H;       // 312
    const lv_coord_t ZONE_MARK_Y = ZONE_BAR_Y - ZONE_MARK_H;   // 304
    const lv_coord_t ROW2 = ZONE_MARK_Y - CELL_H;              // 244
    const lv_coord_t ROW1 = ROW2 - CELL_H;                     // 184

    // Navigation starts at the very top: the status line (gear, clock,
    // battery) sits directly over it with no rule between them, so the two
    // read as one region rather than two stacked widgets.
    const lv_coord_t NAV_Y = 0;
    const lv_coord_t NAV_H = ROW1 - NAV_Y;                     // 184
    const lv_coord_t FULL_W = SCREEN_W;

    // Height the status line occupies inside the navigation region. Not a
    // widget of its own any more -- just the band the turn content keeps
    // clear of.
    const lv_coord_t STATUS_H = 28;


    // ---- Layout ----
    // Navigation dominates: on a bike, the next turn or where the trail goes
    // is what a glance is for. Metrics sit underneath in equal cells, each
    // bounding its own contents.
    //
    // The clock moved into the header, replacing a "PEGASUS" wordmark that
    // told the rider nothing they did not already know -- and that freed a
    // whole cell for a metric.

    // ---- Navigation slot: one slot, two possible occupants ----
    s_nav_is_map = (Settings_GetNavMode() == NAV_MODE_GPX);

    if (s_nav_is_map) {
        // GPX gets the actual map, inline, at the size the glance deserves.
        MapView_Create(&s_map_view, parent, 0, NAV_Y, FULL_W, NAV_H, s_map_points,
                       s_map_projected, INLINE_MAP_POINTS);
        s_nav_cell = s_map_view.container;

        // Roads here too. The dashboard's navigation tile is a MapView like
        // the ROUTE page's, and a rider looking at the dashboard has the same
        // question about which street is which.
        RoadView_Attach(&s_map_view);
        MapView_FitTrack(&s_map_view);
        RoadView_Refresh();

        // Tapping it opens the full-screen map, where the trail gets the
        // whole panel.
        lv_obj_add_flag(s_nav_cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_nav_cell, OnMapClicked, LV_EVENT_CLICKED, this);

        if (GpxTrack_PointCount() == 0) {
            lv_obj_t *empty = lv_label_create(s_nav_cell);
            // Say which of the two reasons applies: a missing card and an
            // unreadable one need different things from the rider.
            lv_label_set_text(empty, GpxTrack_CardMounted() ? "No .gpx on card" : "No SD card");
            lv_obj_set_style_text_font(empty, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(empty, lv_color_hex(COLOR_CAPTION), 0);
            lv_obj_center(empty);
        }
    } else {
        // TBT gets the turn, which is all the phone sends and all a junction
        // needs. It carries the largest face on the screen now: it is the one
        // time-critical thing here.
        // No caption: the status line now occupies this tile's top-left
        // corner, and an arrow with a distance beside it needs no label.
        s_nav_cell = MakeCell(parent, 0, NAV_Y, FULL_W, NAV_H, "");

        // ---- Filling the navigation tile ----
        // 240x184 with the status band across the top. The old layout put a
        // 64px arrow and a 48pt "200 m" side by side and left most of the
        // tile empty, because the combined string needed 155px of width and
        // capped the arrow at whatever was left.
        //
        // Splitting the distance (see FormatTbtDistance) frees that width:
        // the number keeps the largest face LVGL ships and the unit becomes a
        // caption beneath it, so the arrow can take the whole left column.
        s_route_arrow_label = lv_img_create(s_nav_cell);
        lv_img_set_src(s_route_arrow_label, TbtIcon(TBT_ICON_STRAIGHT));
        // recolor_opa must be full or the recolour is a no-op and an
        // ALPHA_8BIT image draws in the theme's default, not the accent.
        lv_obj_set_style_img_recolor_opa(s_route_arrow_label, LV_OPA_COVER, 0);
        lv_obj_set_style_img_recolor(s_route_arrow_label, lv_color_hex(COLOR_ACCENT), 0);
        lv_obj_align(s_route_arrow_label, LV_ALIGN_TOP_LEFT, PAD, STATUS_H - 2);

        // The number, right of the arrow. Right-aligned so the digits stay
        // put as the distance counts down and the string shortens.
        s_route_dist_label = lv_label_create(s_nav_cell);
        lv_obj_set_style_text_font(s_route_dist_label, &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_color(s_route_dist_label, lv_color_hex(COLOR_VALUE), 0);
        lv_label_set_text(s_route_dist_label, "");
        lv_obj_set_style_text_align(s_route_dist_label, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(s_route_dist_label, FULL_W - TBT_ICON_PX - 3 * PAD);
        lv_label_set_long_mode(s_route_dist_label, LV_LABEL_LONG_CLIP);
        lv_obj_align(s_route_dist_label, LV_ALIGN_TOP_RIGHT, -PAD, STATUS_H + 14);

        s_route_dist_unit = lv_label_create(s_nav_cell);
        lv_obj_set_style_text_font(s_route_dist_unit, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(s_route_dist_unit, lv_color_hex(COLOR_ACCENT), 0);
        lv_label_set_text(s_route_dist_unit, "");
        lv_obj_align_to(s_route_dist_unit, s_route_dist_label, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 2);

        // The road name is how a rider confirms the turn, so it gets the full
        // width and the biggest size that still fits a typical name: at 24pt
        // "Rockingham Rd" is 197px of the 228 available. Longer names ellipsize,
        // which beats shrinking every name to suit the worst one.
        s_route_dir_label = lv_label_create(s_nav_cell);
        lv_obj_set_width(s_route_dir_label, FULL_W - 2 * PAD);
        lv_label_set_long_mode(s_route_dir_label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(s_route_dir_label, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(s_route_dir_label, lv_color_hex(COLOR_VALUE), 0);
        lv_obj_align(s_route_dir_label, LV_ALIGN_BOTTOM_LEFT, PAD, -PAD);
    }

    // ---- Status line ----
    // Created after the navigation slot on purpose: it sits over the top of
    // it, and the navigation tile is opaque, so building it first would put
    // these behind it.
    lv_obj_t *settings_btn = lv_btn_create(parent);
    lv_obj_set_size(settings_btn, 34, 24);
    lv_obj_align(settings_btn, LV_ALIGN_TOP_LEFT, 3, 2);
    lv_obj_set_style_bg_color(settings_btn, lv_color_hex(0x1D2A36), 0);
    lv_obj_set_style_bg_color(settings_btn, lv_color_hex(COLOR_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_radius(settings_btn, 6, 0);
    lv_obj_set_style_shadow_width(settings_btn, 0, 0);
    lv_obj_add_event_cb(settings_btn, OnSettingsClicked, LV_EVENT_CLICKED, this);

    lv_obj_t *gear = lv_label_create(settings_btn);
    lv_label_set_text(gear, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(gear, lv_color_hex(COLOR_VALUE), 0);
    lv_obj_center(gear);

    s_clock_label = lv_label_create(parent);
    lv_obj_set_style_text_font(s_clock_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_clock_label, lv_color_hex(COLOR_VALUE), 0);
    lv_label_set_text(s_clock_label, "--:--");
    lv_obj_align(s_clock_label, LV_ALIGN_TOP_MID, -10, 6);

    s_clock_caption = lv_label_create(parent);
    lv_obj_set_style_text_font(s_clock_caption, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_clock_caption, lv_color_hex(COLOR_CAPTION), 0);
    lv_label_set_text(s_clock_caption, "");
    lv_obj_align_to(s_clock_caption, s_clock_label, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -2);

    // ---- Battery ----
    // The device's own battery, published to TOPIC_BATTERY by the Core 0
    // monitor in hal/Battery.cpp -- not HeartRate_t.battery, which is the
    // strap's.
    s_battery_label = MakeLabel(parent, LV_SYMBOL_BATTERY_FULL " --%", &lv_font_montserrat_12,
                                COLOR_CAPTION, LV_ALIGN_TOP_RIGHT, -PAD, 7);


    // ---- Four metrics, two by two ----
    // Speed is one of them now rather than a hero: worth reading, but not at
    // the cost of the turn that is actually approaching.
    //
    // Each value gets the cell's full width now that no unit sits beside it,
    // which is what pays for the jump from 28 to 34. The widest thing any of
    // them has to hold is a six-character trip ("123.45"), and at 34 that
    // comes to about 104px inside 108px of usable width.
    lv_obj_t *speed_cell = MakeCell(parent, COL1, ROW1, CELL_W, CELL_H, "SPEED");
    s_speed_label = MakeValue(speed_cell, "--", COLOR_VALUE);
    s_speed_unit_label = MakeUnit(speed_cell, Settings_SpeedUnitLabel());
    lv_obj_set_style_text_color(s_speed_unit_label, lv_color_hex(COLOR_ACCENT), 0);

    lv_obj_t *trip_cell = MakeCell(parent, COL2, ROW1, CELL_W, CELL_H, "TRIP");
    s_trip_label = MakeValue(trip_cell, "0.00", COLOR_VALUE);
    s_trip_unit_label = MakeUnit(trip_cell, Settings_DistanceUnitLabel());

    s_incline_cell = MakeCell(parent, COL1, ROW2, CELL_W, CELL_H, "INCLINE");
    s_incline_label = MakeValue(s_incline_cell, "--", COLOR_ACCENT);
    MakeUnit(s_incline_cell, "%");

    lv_obj_t *hr_cell = MakeCell(parent, COL2, ROW2, CELL_W, CELL_H, "HEART RATE");
    s_hr_label = MakeValue(hr_cell, "--", COLOR_VALUE);
    MakeUnit(hr_cell, "bpm");

    // ---- Dividing lines ----
    // Internal joins only. Nothing is drawn at x=0, x=239, y=0 or y=319, so
    // each widget runs into the screen edge with no frame around it.
    MakeSeparator(parent, 0, ROW1 - 1, SCREEN_W, 1);      // navigation / metrics
    MakeSeparator(parent, 0, ROW2 - 1, SCREEN_W, 1);      // between metric rows
    MakeSeparator(parent, 0, ZONE_MARK_Y - 1, SCREEN_W, 1); // metrics / zone block
    // One vertical line down the metric block only -- the navigation slot and
    // the zone bar above and below it are full width and must not be cut.
    MakeSeparator(parent, COL2 - 1, ROW1, 1, ROW2 + CELL_H - ROW1);

    // ---- Heart-rate zone block ----
    // Five bands from HrZone.h, scaled to the rider's own resting and maximum
    // rate; the active one is lit and the rest dimmed. Colour and position
    // carry the reading, no word to parse.
    //
    // Equal fifths, not one segment per span. The bands really are unequal in
    // beats -- zone 4 covers 30% of the reserve and zone 5 only 10% -- and
    // drawing that honestly gave a bar of 72/24/48/72/24px in which the narrow
    // zones were hard to tell apart at a glance. Five equal blocks read as a
    // scale. The cost is that the marker can no longer be placed by reserve,
    // which is what HrZone_EqualWidthFraction is for.
    s_zone_bar_w = SCREEN_W;
    lv_coord_t seg_x = 0;
    for (int i = 0; i < HR_ZONE_COUNT; i++) {
        // Edge-to-edge arithmetic rather than a fixed width per segment, so
        // the five always cover exactly SCREEN_W even when it does not divide
        // by five. No seams, no overrun on the last one.
        const lv_coord_t next_x = (lv_coord_t)(((i + 1) * SCREEN_W) / HR_ZONE_COUNT);

        lv_obj_t *segment = lv_obj_create(parent);
        // Touching, not spaced: the five colours already separate them, and
        // the bar reads as one gauge rather than five buttons.
        lv_obj_set_size(segment, next_x - seg_x, ZONE_BAR_H);
        lv_obj_set_pos(segment, seg_x, ZONE_BAR_Y);
        lv_obj_set_style_bg_color(segment, lv_color_hex(ZONE_COLORS[i]), 0);
        lv_obj_set_style_bg_opa(segment, LV_OPA_40, 0);
        lv_obj_set_style_border_width(segment, 0, 0);
        lv_obj_set_style_radius(segment, 2, 0);
        lv_obj_clear_flag(segment, LV_OBJ_FLAG_SCROLLABLE);
        s_zone_segments[i] = segment;

        seg_x = next_x;
    }

    // The triangle, in its own row above the colours so it never covers the
    // band it is pointing at. Widest row at the top, apex at the bottom.
    s_zone_marker = lv_obj_create(parent);
    lv_obj_set_size(s_zone_marker, ZONE_MARKER_W, ZONE_MARKER_H);
    lv_obj_set_pos(s_zone_marker, 0, ZONE_MARK_Y);
    lv_obj_set_style_bg_opa(s_zone_marker, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_zone_marker, 0, 0);
    lv_obj_set_style_pad_all(s_zone_marker, 0, 0);
    lv_obj_clear_flag(s_zone_marker, LV_OBJ_FLAG_SCROLLABLE);

    for (lv_coord_t row = 0; row < ZONE_MARKER_ROWS; row++) {
        const lv_coord_t w = ZONE_MARKER_W - row * ZONE_MARKER_STEP;
        lv_obj_t *step = lv_obj_create(s_zone_marker);
        lv_obj_set_size(step, w, ZONE_MARKER_ROW_H);
        // Centred on the container, so the apex lands on its middle column.
        lv_obj_set_pos(step, (ZONE_MARKER_W - w) / 2, row * ZONE_MARKER_ROW_H);
        // White: it has to read against all five bands and the background.
        lv_obj_set_style_bg_color(step, lv_color_hex(COLOR_VALUE), 0);
        lv_obj_set_style_bg_opa(step, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(step, 0, 0);
        lv_obj_set_style_radius(step, 0, 0);
        lv_obj_clear_flag(step, LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_obj_add_flag(s_zone_marker, LV_OBJ_FLAG_HIDDEN); // nothing to point at yet

    if (!s_nav_is_map) {
        ClearTbt();
    }

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
    s_route_arrow_label = nullptr;
    s_route_dir_label = nullptr;
    s_route_dist_label = nullptr;
    s_route_dist_unit = nullptr;
    s_trip_unit_label = nullptr;
    s_incline_cell = nullptr;
    s_nav_cell = nullptr;
    s_nav_is_map = false;
    for (int i = 0; i < HR_ZONE_COUNT; i++) {
        s_zone_segments[i] = nullptr;
    }
    s_zone_marker = nullptr;
    s_zone_bar_w = 0;
    s_battery_label = nullptr;
}
