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
#include "../navigation/RideLog.h"
#include "../system/RideStats.h"
#include "../system/Settings.h"
#include "../system/Trip.h"
#include "../navigation/GpxTrack.h"
#include "../navigation/TbtParse.h"
#include "../system/TimeSource.h"
#include "../system/TimeZone.h"
#include "MapView.h"
#include "RoadView.h"
#include "Page_Map.h"
#include "NumFont.h"
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

// The turn is close enough to act on. Green rather than another warning
// colour: amber and red are already spoken for above and both mean "something
// is less certain", where this means the opposite -- act now.
constexpr uint32_t COLOR_NAV_IMMINENT = 0x7CE38B;

// The countdown bar's unfilled track. Light enough to read as a track on the
// tile's dark background, dark enough not to compete with the arrow.
constexpr uint32_t COLOR_BAR_TRACK = 0x33475B;

// Distance at which a turn stops being something to expect and becomes
// something to do. At 25 km/h this is about four seconds of warning, which is
// roughly the point where a rider should already be in the right lane.
constexpr uint32_t TBT_IMMINENT_M = 30;

// How far out a fresh maneuver fills the countdown bar. A turn announced
// further away than this simply starts at full; without a ceiling, one
// announced 5 km out would leave the bar visibly motionless for most of a
// ride and teach the rider to ignore it.
constexpr uint32_t TBT_BAR_MAX_M = 500;

// The arrow asset's own size. The bitmaps are generated at exactly the size
// they are drawn at (tools/genicons.py takes it as an argument), because
// scaling them at runtime with lv_img_set_zoom made the arrow disappear
// entirely -- a transformed ALPHA_8BIT image drew nothing on this build.
constexpr lv_coord_t TBT_ARROW_DRAW_PX = TBT_ICON_PX;

// Usable width inside the navigation tile: the 240px panel less the tile's
// padding on both sides. At file scope because the street name is re-fitted
// on every directive, long after Create()'s locals have gone.
constexpr lv_coord_t TBT_TEXT_W = 240 - 2 * 6;
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
lv_obj_t *s_incline_unit_label = nullptr;
lv_obj_t *s_clock_label = nullptr;
lv_obj_t *s_clock_caption = nullptr;

// Applying a TZ string calls tzset(), which is not free, so only redo it when
// the zone actually changes -- which is almost never on a bike.
const char *s_active_tz = nullptr;
lv_obj_t *s_incline_label = nullptr;
lv_obj_t *s_hr_label = nullptr;
// Ride averages and peaks, beside the live value in the two tall cells.
lv_obj_t *s_speed_avg_label = nullptr;
lv_obj_t *s_speed_max_label = nullptr;
lv_obj_t *s_hr_avg_label = nullptr;
lv_obj_t *s_hr_max_label = nullptr;
lv_obj_t *s_incline_cell = nullptr;
// The caption of the cell above, because it names two different things.
//
// This board has no IMU (CLAUDE.md section 2), so INCLINE has shown "--" for
// the life of the project and always will. Rather than keep a dead cell and
// find nowhere for total ascent, the cell reports ascent until an IMU
// actually publishes, and grade afterwards. The caption says which, so it is
// never ambiguous, and on any given board it settles one way and stays there.
lv_obj_t *s_incline_caption = nullptr;
bool s_have_imu = false;
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
// The navigation tile's contents, packed top to bottom by a flex column so a
// row with nothing to say can hide itself and give its height back.
lv_obj_t *s_nav_content = nullptr;
lv_obj_t *s_route_dist_row = nullptr;
lv_obj_t *s_route_dist_label = nullptr;
lv_obj_t *s_route_dist_unit = nullptr;
// Drains as the rider closes on the turn. A number has to be read; a bar is
// understood while looking at the road.
lv_obj_t *s_route_bar = nullptr;
// Which exit to take, drawn over the middle of the roundabout arrow. Every
// roundabout shares one icon, so without this they are indistinguishable.
lv_obj_t *s_route_exit_label = nullptr;
lv_obj_t *s_route_secondary_row = nullptr;
lv_obj_t *s_route_then_label = nullptr;
lv_obj_t *s_route_remaining_label = nullptr;

// The distance this maneuver was first announced at, which is what the
// countdown bar is scaled against. Reset whenever the maneuver changes, and
// kept here rather than recomputed because the directive carries no history.
uint32_t s_tbt_bar_scale_m = 0;
uint8_t s_tbt_bar_icon = 0;
char s_tbt_bar_street[TBT_STREET_NAME_MAX] = {0};
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
// When the GNSS-with-position path last drew the clock. That path resolves the
// zone from the fix itself and applies real daylight-saving rules, so it is
// left to win while it is running; RenderClock() only takes over once it has
// gone quiet.
uint32_t s_clock_from_fix_ms = 0;
uint32_t s_clock_drawn_ms = 0;

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

// The average-and-peak block beside a live value. 62px is "MAX" at 10pt and a
// four-character figure at 16pt with a few pixels between them; 20px a row is
// that line box plus the gap that keeps two of them from touching.
//
// Inset 2 rather than the captions' 6: nearly against the cell edge, because
// every pixel this block gives up goes to the live figure, which is the one
// read at speed.
constexpr lv_coord_t SECONDARY_W = 62;
constexpr lv_coord_t SECONDARY_ROW_H = 20;
constexpr lv_coord_t SECONDARY_INSET = 2;

// How far the word sits in from the block's left edge. The figures are pinned
// to the right, so this is really the gap between "AVG" and the number it
// names: the wider it is, the more the pair reads as two separate things
// rather than one label.
//
// 4 is the ceiling, set by the widest figure the block ever holds. At 6 the
// speed cell's "AVG" ran into "24.6" and the two read as one token; a
// three-digit heart rate leaves more room and would take 8.
constexpr lv_coord_t SECONDARY_WORD_X = 4;
constexpr lv_coord_t CELL_CAPTION_Y = 2;   // caption and unit baseline row
constexpr lv_coord_t CELL_VALUE_Y = -1;    // value, up from the cell's bottom

// One bordered cell: caption at the top, value at the bottom. Cells bound
// their contents, so a long value cannot drift into a neighbour -- which is
// exactly how the clock ended up on top of the incline figure when these were
// free-floating labels.
lv_obj_t *MakeCell(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                   const char *caption, lv_obj_t **out_caption = nullptr) {
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
    if (out_caption != nullptr) {
        *out_caption = label;
    }
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
lv_obj_t *MakeValueIn(lv_obj_t *cell, const char *text, uint32_t color,
                      const lv_font_t *font) {
    lv_obj_t *label = lv_label_create(cell);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, CELL_PAD, CELL_VALUE_Y);
    return label;
}

lv_obj_t *MakeValue(lv_obj_t *cell, const char *text, uint32_t color) {
    return MakeValueIn(cell, text, color, &lv_font_montserrat_40);
}

// One line of the ride block: a small word, then the figure it names.
//
// Two labels rather than one string, because they are not the same kind of
// thing. "AVG" is a caption and the number beside it is a reading, and LVGL
// cannot give one label two sizes -- so a single "AVG 142" forces the word to
// be as large as the figure, which is backwards. The row is flex, so the word
// stays put as the figure changes width.
lv_obj_t *MakeSecondaryRow(lv_obj_t *parent, const char *word) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    // Explicit width, not LV_SIZE_CONTENT. Nested content-sized flex
    // containers did not resolve here: the row took the width of the figure
    // alone and the word beside it was clipped to a two-pixel sliver of its
    // last letter. A width decided in the layout cannot be got wrong by a
    // measuring pass that runs in the wrong order.
    lv_obj_set_size(row, lv_pct(100), SECONDARY_ROW_H);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    // Not clickable: lv_obj_create sets that flag and this is a container,
    // not a control. A transparent box that answers touches is how the road
    // layer once swallowed every tap meant for the map beneath it.
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    // Placed rather than flexed, and for once that is the simpler answer:
    // there are two children, one pinned to each end, and lv_obj_align stores
    // the alignment so it survives the labels changing width.
    lv_obj_t *caption = lv_label_create(row);
    lv_label_set_text(caption, word);
    lv_obj_set_style_text_font(caption, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(caption, lv_color_hex(COLOR_CAPTION), 0);
    lv_obj_align(caption, LV_ALIGN_BOTTOM_LEFT, SECONDARY_WORD_X, -3);

    lv_obj_t *value = lv_label_create(row);
    lv_label_set_text(value, "--");
    // 16pt, down one step from 18. The speed cell is the tightest thing on the
    // panel -- a 40pt live figure, a caption word and this, inside 150px -- and
    // at 18 every gap in it came out at a pixel or two. A step here costs less
    // than a step off the figure read at speed.
    lv_obj_set_style_text_font(value, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(value, lv_color_hex(COLOR_VALUE), 0);
    lv_obj_align(value, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    return value;
}

// The average-and-peak block, bottom-right of a cell beside the live value.
// Writes the two figure labels back through the pointers; the words are fixed
// and nothing needs to hold them.
void MakeSecondary(lv_obj_t *cell, lv_obj_t **out_avg, lv_obj_t **out_max) {
    lv_obj_t *block = lv_obj_create(cell);
    lv_obj_remove_style_all(block);
    // Wide enough for "MAX" and a three-digit figure, and no wider: the live
    // value has the rest of the cell and the two must not meet.
    lv_obj_set_size(block, SECONDARY_W, 2 * SECONDARY_ROW_H);
    lv_obj_clear_flag(block, LV_OBJ_FLAG_SCROLLABLE);
    // Not clickable: lv_obj_create sets that flag and this is a container,
    // not a control. A transparent box that answers touches is how the road
    // layer once swallowed every tap meant for the map beneath it.
    lv_obj_clear_flag(block, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(block, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_align(block, LV_ALIGN_BOTTOM_RIGHT, -SECONDARY_INSET, CELL_VALUE_Y);

    *out_avg = MakeSecondaryRow(block, "AVG");
    *out_max = MakeSecondaryRow(block, "MAX");
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

// The maneuver in words, for the "then" preview.
//
// Words, not a second arrow. A preview arrow would have to be drawn at about
// a fifth of the asset's size, which downsamples an already small shape into
// mush -- and "then left" is unambiguous in a way a 20px glyph is not.
const char *ManeuverWord(uint8_t icon_id) {
    switch (icon_id) {
        case TBT_ICON_TURN_LEFT: return "left";
        case TBT_ICON_TURN_RIGHT: return "right";
        case TBT_ICON_SLIGHT_LEFT: return "bear left";
        case TBT_ICON_SLIGHT_RIGHT: return "bear right";
        case TBT_ICON_SHARP_LEFT: return "sharp left";
        case TBT_ICON_SHARP_RIGHT: return "sharp right";
        case TBT_ICON_UTURN: return "U-turn";
        case TBT_ICON_ROUNDABOUT: return "roundabout";
        case TBT_ICON_ARRIVE: return "arrive";
        // "ahead", not "straight on": the same instruction in six characters
        // rather than eleven, on the one line that keeps running out of room.
        case TBT_ICON_STRAIGHT: return "ahead";
        default: return "";
    }
}

// Picks the largest font the name actually fits in, rather than shrinking
// every name to suit the longest one.
//
// "Rockingham Road" ellipsized to "Rockingham..." on the bench, and the street
// name is how a rider confirms they are turning where they meant to -- a name
// that has lost its second half cannot do that. Scrolling text was the
// alternative and is worse: movement at the edge of vision while riding is a
// distraction, and the name is only readable during part of the cycle.
void SetStreetName(lv_obj_t *label, const char *text, lv_coord_t max_width) {
    static const lv_font_t *const kFonts[] = {
        &lv_font_montserrat_24,
        &lv_font_montserrat_18,
        &lv_font_montserrat_14,
    };
    const lv_font_t *chosen = kFonts[sizeof(kFonts) / sizeof(kFonts[0]) - 1];
    for (size_t i = 0; i < sizeof(kFonts) / sizeof(kFonts[0]); i++) {
        lv_point_t size;
        lv_txt_get_size(&size, text, kFonts[i], 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x <= max_width) {
            chosen = kFonts[i];
            break;
        }
    }
    lv_obj_set_style_text_font(label, chosen, 0);
    lv_label_set_text(label, text);
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
    lv_obj_set_style_text_font(s_route_dir_label, &lv_font_montserrat_24, 0);
    lv_bar_set_value(s_route_bar, 0, LV_ANIM_OFF);
    lv_obj_add_flag(s_route_exit_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_route_secondary_row, LV_OBJ_FLAG_HIDDEN);
    // Forget the bar's scale too, or the next route's first turn is measured
    // against a maneuver from the last one.
    s_tbt_bar_scale_m = 0;
    s_tbt_bar_icon = 0;
    s_tbt_bar_street[0] = '\0';
}

// Draws the clock from whichever source currently outranks the others.
//
// Called on a timer rather than only when a fix arrives, because the phone is
// now a source too and it writes minutes apart -- a clock that only redrew on
// a GNSS publish would sit at the same minute for the whole gap, or at dashes
// forever on a head unit that has never seen a satellite.
//
// The GNSS-with-position path above is left alone and still wins when it
// applies: resolving the zone from the fix gives real daylight-saving rules,
// where the phone can only state the offset it happens to be using.
// A speed or a ride average, at a precision the cell can actually hold.
//
// One decimal below 100 and none above, which is the rule the trip readout
// already uses. The speed cell shares 150px between a 32pt live value and two
// 18pt ride figures, and that fits at four characters and does not at five:
// the simulator drew "105.3" straight through "AVG 104.2". A tenth of a km/h
// stops being worth anything a long way before three digits, and a bad fix
// reporting 400 is the case that reaches them.
void FormatMetric(float value, char *out, size_t size) {
    snprintf(out, size, (value >= 100.0f || value <= -100.0f) ? "%.0f" : "%.1f", value);
}

void RenderClock() {
    TimeReading_t reading;
    if (!TimeSource_Now(lv_tick_get(), &reading)) {
        lv_label_set_text(s_clock_label, "--:--");
        lv_label_set_text(s_clock_caption, "");
        return;
    }

    // Applied by arithmetic, not by setenv/tzset. The offset is already
    // resolved -- the phone sent the one it is actually using, daylight saving
    // included -- so there is no rule to evaluate, and the zone database this
    // firmware carries is a coarse position lookup that would only second-
    // guess it.
    const int32_t local_seconds =
        (int32_t)(reading.utc_seconds % 86400u) + (int32_t)reading.offset_min * 60;
    // The offset can push the local day either side of the UTC one.
    const int32_t wrapped = ((local_seconds % 86400) + 86400) % 86400;

    lv_label_set_text_fmt(s_clock_label, "%02d:%02d", (int)(wrapped / 3600),
                          (int)((wrapped % 3600) / 60));

    if (!reading.offset_known) {
        // A time nobody has placed in a zone. Saying UTC is honest; showing
        // it as local would be a guess presented as a fact.
        lv_label_set_text(s_clock_caption, "UTC");
    } else if (reading.zone[0] != '\0') {
        lv_label_set_text(s_clock_caption, reading.zone);
    } else {
        // No abbreviation, so state the offset itself rather than nothing.
        const int mins = reading.offset_min;
        lv_label_set_text_fmt(s_clock_caption, "%+03d:%02d", mins / 60, abs(mins) % 60);
    }
}

void RenderSpeedAndTrip() {
    if (s_has_speed) {
        char speed[12];
        FormatMetric(Settings_SpeedFromKmh(s_last_speed_kmh), speed, sizeof(speed));
        lv_label_set_text(s_speed_label, speed);
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

    if (s_speed_avg_label != nullptr) {
        const float max_kmh = RideStats_MaxSpeedKmh();
        if (max_kmh <= 0.0f) {
            // Dashes, not zeros, before anything has moved. Zero reads as a
            // ride that went nowhere, where dashes read as one that has not
            // started.
            lv_label_set_text(s_speed_avg_label, "--");
            lv_label_set_text(s_speed_max_label, "--");
        } else {
            char avg[12];
            char max[12];
            FormatMetric(Settings_SpeedFromKmh(RideStats_AvgSpeedKmh()), avg, sizeof(avg));
            FormatMetric(Settings_SpeedFromKmh(max_kmh), max, sizeof(max));
            lv_label_set_text(s_speed_avg_label, avg);
            lv_label_set_text(s_speed_max_label, max);
        }
    }
}

void RenderHeartRateStats() {
    if (s_hr_avg_label == nullptr) {
        return;
    }
    const uint8_t avg = RideStats_AvgBpm();
    const uint8_t max = RideStats_MaxBpm();

    // Dashes rather than zero before a strap has reported. Zero is a number a
    // rider could believe, and "average heart rate 0" reads as a fault rather
    // than as an absence.
    if (avg == 0) {
        lv_label_set_text(s_hr_avg_label, "--");
        lv_label_set_text(s_hr_max_label, "--");
        lv_obj_set_style_text_color(s_hr_avg_label, lv_color_hex(COLOR_VALUE), 0);
        lv_obj_set_style_text_color(s_hr_max_label, lv_color_hex(COLOR_VALUE), 0);
        return;
    }

    lv_label_set_text_fmt(s_hr_avg_label, "%u", (unsigned)avg);
    lv_label_set_text_fmt(s_hr_max_label, "%u", (unsigned)max);

    // Each figure takes ITS OWN zone's colour, not the live reading's. A ride
    // that averages zone 2 and peaks in zone 5 is the ordinary shape of a
    // ride, and colouring both by the current beat would hide exactly that.
    // The live value already works this way; this extends the same encoding to
    // the two figures beside it, so the cell reads as three zones at a glance
    // with no number parsed.
    const uint8_t rest = Settings_GetHrRestBpm();
    const uint8_t ceiling = Settings_GetHrMaxBpm();
    lv_obj_set_style_text_color(
        s_hr_avg_label, lv_color_hex(ZONE_COLORS[HrZone_Index(avg, rest, ceiling)]), 0);
    lv_obj_set_style_text_color(
        s_hr_max_label, lv_color_hex(ZONE_COLORS[HrZone_Index(max, rest, ceiling)]), 0);
}

// The only place in this file allowed to touch LVGL objects: an lv_timer
// callback runs exclusively from lv_timer_handler(), which this project only
// ever calls from lvgl_task() on Core 1 (see src/system/LvglTask.cpp).
void RefreshTimerCallback(lv_timer_t *timer) {
    (void)timer;

    // Once a second, and only when a positioned fix is not already drawing it.
    // Every second rather than every 100ms because the display shows minutes,
    // and redrawing a label that has not changed ten times a second is work
    // for nothing.
    if (lv_tick_elaps(s_clock_drawn_ms) >= 1000) {
        s_clock_drawn_ms = lv_tick_get();

        // The averages move slowly and, more to the point, nothing publishes
        // while the rider is stopped -- so a block redrawn only on a GPS or
        // heart-rate publish would freeze exactly when someone is standing
        // over the bike reading it.
        RenderSpeedAndTrip();
        RenderHeartRateStats();
        const bool fix_is_drawing =
            s_clock_from_fix_ms != 0 && lv_tick_elaps(s_clock_from_fix_ms) < 3000;
        if (!fix_is_drawing) {
            RenderClock();
        }
    }

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
            // Into the ranking before it is drawn, so a later tick without a
            // fix can still show the time -- and so the phone's offset is
            // available to a fix that has no idea what zone it is over.
            if (gps.time_valid) {
                TimeSource_SetFromGnss(
                    (uint32_t)TimeZone_UtcToEpoch(gps.year, gps.month, gps.day, gps.hour,
                                                  gps.minute, gps.second),
                    lv_tick_get());
            }

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
                s_clock_from_fix_ms = lv_tick_get();
            }
            // No dedicated "time but no fix" branch any more. That case now
            // falls through to RenderClock() below, which can do better than
            // the UTC this used to show: the phone's offset outlives the
            // connection that delivered it, so a fix with no position can
            // still be displayed in the rider's own zone.
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
                // Close enough to act on. Overrides the source colour: which
                // module produced the turn stops mattering when the junction
                // is four seconds away.
                const bool imminent = tbt.distance_m != TBT_DISTANCE_UNKNOWN &&
                                      tbt.distance_m <= TBT_IMMINENT_M;
                if (imminent) {
                    arrow_colour = COLOR_NAV_IMMINENT;
                }

                lv_img_set_src(s_route_arrow_label, TbtIcon(tbt.icon_id));
                lv_obj_set_style_img_recolor(s_route_arrow_label, lv_color_hex(arrow_colour), 0);
                lv_label_set_text(s_route_dist_label, dist);
                lv_label_set_text(s_route_dist_unit, dist_unit);
                lv_obj_set_style_text_color(s_route_dist_label,
                                            lv_color_hex(imminent ? COLOR_NAV_IMMINENT
                                                                  : COLOR_VALUE),
                                            0);

                const char *street =
                    tbt.street_name[0] != '\0' ? tbt.street_name : "AHEAD";
                SetStreetName(s_route_dir_label, street, TBT_TEXT_W);

                // ---- The countdown bar ----
                // A new maneuver resets the scale to whatever distance it was
                // announced at, so the bar always starts full and drains to
                // the junction. Detected by icon or street changing: the
                // directive carries no maneuver id, and the distance alone
                // cannot tell a new turn from the old one counting down.
                const bool new_maneuver = tbt.icon_id != s_tbt_bar_icon ||
                                          strncmp(street, s_tbt_bar_street,
                                                  sizeof(s_tbt_bar_street)) != 0;
                if (new_maneuver) {
                    s_tbt_bar_icon = tbt.icon_id;
                    strncpy(s_tbt_bar_street, street, sizeof(s_tbt_bar_street) - 1);
                    s_tbt_bar_street[sizeof(s_tbt_bar_street) - 1] = '\0';
                    s_tbt_bar_scale_m = tbt.distance_m == TBT_DISTANCE_UNKNOWN
                                            ? 0
                                            : (tbt.distance_m > TBT_BAR_MAX_M ? TBT_BAR_MAX_M
                                                                              : tbt.distance_m);
                }
                if (s_tbt_bar_scale_m == 0 || tbt.distance_m == TBT_DISTANCE_UNKNOWN) {
                    // Nothing honest to draw: an empty bar, not a full one,
                    // because a full bar reads as "miles to go" rather than
                    // "unknown".
                    lv_bar_set_value(s_route_bar, 0, LV_ANIM_OFF);
                } else {
                    uint32_t left = tbt.distance_m > s_tbt_bar_scale_m ? s_tbt_bar_scale_m
                                                                      : tbt.distance_m;
                    const int32_t filled =
                        (int32_t)(1000 - (left * 1000) / s_tbt_bar_scale_m);
                    lv_bar_set_value(s_route_bar, filled, LV_ANIM_OFF);
                }
                lv_obj_set_style_bg_color(s_route_bar,
                                          lv_color_hex(imminent ? COLOR_NAV_IMMINENT
                                                                : arrow_colour),
                                          LV_PART_INDICATOR);

                // ---- Which exit, over the arrow ----
                if (tbt.exit_number > 0) {
                    lv_label_set_text_fmt(s_route_exit_label, "%u",
                                          (unsigned)tbt.exit_number);
                    lv_obj_clear_flag(s_route_exit_label, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(s_route_exit_label, LV_OBJ_FLAG_HIDDEN);
                }

                // ---- What follows, and how far is left ----
                // Only the cached route knows either, so both are blank until
                // a route has been uploaded and the rider placed on it. The
                // row hides itself when neither has anything, and a flex
                // column gives its height back to the rows that do.
                bool any_secondary = false;
                if (tbt.then_icon_id != TBT_ICON_NONE) {
                    char then_dist[16];
                    char then_unit[8];
                    FormatTbtDistance(tbt.then_distance_m, then_dist, sizeof(then_dist),
                                      then_unit, sizeof(then_unit));
                    // "then right 60 m", not "then right in 60 m". The
                    // preposition is two characters of meaning and eight of
                    // width on a line that has none to spare.
                    lv_label_set_text_fmt(s_route_then_label, "then %s %s%s",
                                          ManeuverWord(tbt.then_icon_id), then_dist,
                                          then_unit);
                    any_secondary = true;
                } else {
                    lv_label_set_text(s_route_then_label, "");
                }
                if (tbt.remaining_m != TBT_DISTANCE_UNKNOWN) {
                    char left_dist[16];
                    char left_unit[8];
                    FormatTbtDistance(tbt.remaining_m, left_dist, sizeof(left_dist),
                                      left_unit, sizeof(left_unit));
                    lv_label_set_text_fmt(s_route_remaining_label, "%s%s left", left_dist,
                                          left_unit);
                    any_secondary = true;
                } else {
                    lv_label_set_text(s_route_remaining_label, "");
                }
                if (any_secondary) {
                    lv_obj_clear_flag(s_route_secondary_row, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(s_route_secondary_row, LV_OBJ_FLAG_HIDDEN);
                }

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
            // An IMU exists after all, so the cell changes what it reports --
            // once, permanently, and says so in its own caption.
            if (!s_have_imu) {
                s_have_imu = true;
                lv_label_set_text(s_incline_caption, "INCLINE");
                lv_label_set_text(s_incline_unit_label, "%");
            }
            // Grade as a percentage of rise over run, from the IMU's pitch.
            const float grade = tanf(imu.pitch * (float)M_PI / 180.0f) * 100.0f;
            lv_label_set_text_fmt(s_incline_label, "%+.1f", grade);
            lv_obj_set_style_bg_color(
                s_incline_cell, lv_color_hex(grade >= 3.0f ? COLOR_CLIMB_FILL : COLOR_CELL_BG), 0);
        }
    }

    // Metres only, and no decimal. Ascent is accurate to a few metres at best,
    // so a tenth would be false precision, and feet would need a unit switch
    // this cell has no room for.
    if (!s_have_imu && s_incline_label != nullptr) {
        lv_label_set_text_fmt(s_incline_label, "%d", (int)(RideStats_AscentM() + 0.5f));
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

void PageDashboard::onViewWillAppear() {
    // Adopt whatever the route page was showing, so the small map does not
    // snap back to its own framing the moment the big one is closed.
    if (s_nav_is_map) {
        MapView_RestoreCamera(&s_map_view);
        RoadView_Refresh();
    }
}

void PageDashboard::onViewDidDisappear() {
    if (s_nav_is_map) {
        MapView_SaveCamera(&s_map_view);
    }
}

bool Page_Dashboard_StartNewRide() {
    Trip_Reset();
    // The averages and maxima describe the same ride as the distance, so they
    // go with it. Leaving a maximum speed behind after a reset would report
    // last week's descent as part of today's commute.
    RideStats_Reset();
    // ...and so does the file on the card, for the same reason. Before this,
    // resetting the odometer left the log running, so the file and the numbers
    // on screen disagreed about which ride the rider was on.
    const bool log_split = RideLog_StartNewRide();
    if (s_trip_label != nullptr) {
        RenderSpeedAndTrip();
    }
    return log_split;
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
    // Two unequal columns, not the even split this used to be.
    //
    // Speed and heart rate share the left one and it is wider, because those
    // two now carry a ride average and a peak beside the live number and the
    // other two do not. Trip and incline are a glance rather than a readout:
    // they keep their own column and take a smaller face, which is what pays
    // for the width the left column gains.
    const lv_coord_t STATS_W = 150;
    const lv_coord_t SEC_W = SCREEN_W - STATS_W;               // 92
    const lv_coord_t COL1 = 0;
    const lv_coord_t COL2 = STATS_W;

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
        // ---- The tile's contents, as one flex column ----
        //
        // Packed by LVGL rather than by hand-placed alignments. Two earlier
        // versions of this tile were positioned with lv_obj_align_to() and
        // absolute offsets, and both broke: the first put the unit off the
        // edge because align_to reads coordinates that the layout pass has not
        // written yet, the second clipped the number because a content-sized
        // box cannot grow into space the arrow already occupies.
        //
        // A column also solves the harder problem here. The "then" preview and
        // the distance to go only exist when a route has been uploaded AND a
        // fix has been taken, which is neither the common case today nor
        // something the tile can be sized for. Hidden children take no space in
        // a flex layout, so the same tile packs correctly with two rows or
        // four, with no branch in the code that fills it.
        s_nav_content = lv_obj_create(s_nav_cell);
        lv_obj_remove_style_all(s_nav_content);
        // Inset less on the left than on the right. The arrow sits at this
        // edge and had more air around it than the tile could spare, while
        // the distance on the other side is right-aligned and stays put
        // because the width is reduced by the same amount it gains.
        const lv_coord_t NAV_PAD_L = 2;
        lv_obj_set_pos(s_nav_content, NAV_PAD_L, STATUS_H);
        lv_obj_set_size(s_nav_content, FULL_W - NAV_PAD_L - PAD, NAV_H - STATUS_H - PAD);
        lv_obj_clear_flag(s_nav_content, LV_OBJ_FLAG_SCROLLABLE);
        // Not clickable: lv_obj_create sets that flag and this is a container,
        // not a control. A transparent box that answers touches is how the road
        // layer once swallowed every tap meant for the map beneath it.
        lv_obj_clear_flag(s_nav_content, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(s_nav_content, LV_FLEX_FLOW_COLUMN);
        // CENTER, not SPACE_BETWEEN.
        //
        // SPACE_BETWEEN spreads the spare height between the rows, which is
        // reasonable when all four are showing and wrong when two are hidden:
        // on the bench every pixel the missing rows freed went into one gap
        // between the number and the street name, and the arrow was pushed up
        // under the clock.
        //
        // Centring keeps the rows together as one block and puts the slack
        // outside it, so the tile reads the same whether it is showing two
        // rows or four.
        lv_obj_set_flex_align(s_nav_content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        // A deliberate gap between rows, now that the layout is not
        // manufacturing one.
        lv_obj_set_style_pad_row(s_nav_content, 6, 0);

        // ---- Row 1: the arrow, and the distance to it ----
        lv_obj_t *turn_row = lv_obj_create(s_nav_content);
        lv_obj_remove_style_all(turn_row);
        lv_obj_set_size(turn_row, lv_pct(100), TBT_ARROW_DRAW_PX);
        lv_obj_clear_flag(turn_row, LV_OBJ_FLAG_SCROLLABLE);
        // Not clickable: lv_obj_create sets that flag and this is a container,
        // not a control. A transparent box that answers touches is how the road
        // layer once swallowed every tap meant for the map beneath it.
        lv_obj_clear_flag(turn_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(turn_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(turn_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        s_route_arrow_label = lv_img_create(turn_row);
        lv_img_set_src(s_route_arrow_label, TbtIcon(TBT_ICON_STRAIGHT));
        // recolor_opa must be full or the recolour is a no-op and an
        // ALPHA_8BIT image draws in the theme's default, not the accent.
        lv_obj_set_style_img_recolor_opa(s_route_arrow_label, LV_OPA_COVER, 0);
        lv_obj_set_style_img_recolor(s_route_arrow_label, lv_color_hex(COLOR_ACCENT), 0);

        // Which exit, in the middle of the roundabout. Costs no layout height,
        // which is the only reason it fits on a tile this full.
        //
        // A CHILD of the arrow, centred, rather than a sibling aligned to it.
        // Aligning to it resolved once, before flex had placed the arrow, so
        // the number landed to the right of the icon instead of inside it --
        // the same one-shot trap that put the distance's unit off the tile and
        // the clock's zone on top of the time. A child is positioned relative
        // to its parent on every layout pass, and follows the arrow for free.
        s_route_exit_label = lv_label_create(s_route_arrow_label);
        lv_obj_set_style_text_font(s_route_exit_label, &lv_font_montserrat_24, 0);
        // White, not the tile's background colour. The roundabout icon is a
        // ring, so its middle is transparent and dark text there is dark text
        // on a dark tile -- which the simulator rendered as no number at all.
        lv_obj_set_style_text_color(s_route_exit_label, lv_color_hex(COLOR_VALUE), 0);
        lv_label_set_text(s_route_exit_label, "");
        lv_obj_add_flag(s_route_exit_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_center(s_route_exit_label);

        // The number over its unit, both right-aligned so the digits stay put
        // as the distance counts down and the string shortens.
        s_route_dist_row = lv_obj_create(turn_row);
        lv_obj_remove_style_all(s_route_dist_row);
        lv_obj_set_size(s_route_dist_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_clear_flag(s_route_dist_row, LV_OBJ_FLAG_SCROLLABLE);
        // Not clickable: lv_obj_create sets that flag and this is a container,
        // not a control. A transparent box that answers touches is how the road
        // layer once swallowed every tap meant for the map beneath it.
        lv_obj_clear_flag(s_route_dist_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(s_route_dist_row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_route_dist_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_END);

        s_route_dist_label = lv_label_create(s_route_dist_row);
        lv_obj_set_style_text_font(s_route_dist_label, &pegasus_font_num_88, 0);
        lv_obj_set_style_text_color(s_route_dist_label, lv_color_hex(COLOR_VALUE), 0);
        lv_label_set_text(s_route_dist_label, "");

        s_route_dist_unit = lv_label_create(s_route_dist_row);
        lv_obj_set_style_text_font(s_route_dist_unit, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(s_route_dist_unit, lv_color_hex(COLOR_ACCENT), 0);
        lv_label_set_text(s_route_dist_unit, "");
        // No negative pad any more. The generated face's line box is measured
        // from the glyphs it actually contains, so it carries no descender
        // space to claw back.

        // ---- Row 2: the countdown bar ----
        s_route_bar = lv_bar_create(s_nav_content);
        lv_obj_set_size(s_route_bar, lv_pct(100), 6);
        lv_bar_set_range(s_route_bar, 0, 1000);
        lv_bar_set_value(s_route_bar, 0, LV_ANIM_OFF);
        // The unfilled track needs to be visible as a track. At the cell
        // border colour it vanished into the background, and 6px of invisible
        // widget between the number and the street name just read as more gap.
        lv_obj_set_style_bg_color(s_route_bar, lv_color_hex(COLOR_BAR_TRACK), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_route_bar, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
        lv_obj_set_style_radius(s_route_bar, 3, LV_PART_MAIN);
        lv_obj_set_style_radius(s_route_bar, 3, LV_PART_INDICATOR);

        // ---- Row 3: what follows the turn, and how far is left ----
        // Both come from the cached route, so both are hidden until one has
        // been uploaded and the head unit has a fix to place the rider on it.
        s_route_secondary_row = lv_obj_create(s_nav_content);
        lv_obj_remove_style_all(s_route_secondary_row);
        lv_obj_set_size(s_route_secondary_row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_clear_flag(s_route_secondary_row, LV_OBJ_FLAG_SCROLLABLE);
        // Not clickable: lv_obj_create sets that flag and this is a container,
        // not a control. A transparent box that answers touches is how the road
        // layer once swallowed every tap meant for the map beneath it.
        lv_obj_clear_flag(s_route_secondary_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(s_route_secondary_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(s_route_secondary_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(s_route_secondary_row, LV_OBJ_FLAG_HIDDEN);

        // Both are given a share of the width and told to ellipsize.
        //
        // SPACE_BETWEEN alone does not stop two labels colliding: when their
        // combined width exceeds the row, flex lets them overlap and the text
        // is drawn on top of itself. The simulator caught exactly that --
        // "then straight on in 800 m" and "11.0 km left" printed through each
        // other. A width each makes the failure a clipped word instead.
        s_route_then_label = lv_label_create(s_route_secondary_row);
        lv_obj_set_style_text_font(s_route_then_label, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(s_route_then_label, lv_color_hex(COLOR_CAPTION), 0);
        lv_obj_set_width(s_route_then_label, lv_pct(60));
        lv_label_set_long_mode(s_route_then_label, LV_LABEL_LONG_DOT);
        lv_label_set_text(s_route_then_label, "");

        s_route_remaining_label = lv_label_create(s_route_secondary_row);
        lv_obj_set_style_text_font(s_route_remaining_label, &lv_font_montserrat_12, 0);
        lv_obj_set_width(s_route_remaining_label, lv_pct(38));
        lv_label_set_long_mode(s_route_remaining_label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(s_route_remaining_label, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_color(s_route_remaining_label, lv_color_hex(COLOR_CAPTION), 0);
        lv_label_set_text(s_route_remaining_label, "");

        // ---- Row 4: the street name ----
        // The full width, and the largest font the name fits in; see
        // SetStreetName for why it is not simply ellipsized.
        s_route_dir_label = lv_label_create(s_nav_content);
        lv_obj_set_width(s_route_dir_label, lv_pct(100));
        lv_label_set_long_mode(s_route_dir_label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(s_route_dir_label, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(s_route_dir_label, lv_color_hex(COLOR_VALUE), 0);
        lv_obj_set_style_text_align(s_route_dir_label, LV_TEXT_ALIGN_CENTER, 0);
    }

    // ---- Status line ----
    // Created after the navigation slot on purpose: it sits over the top of
    // it, and the navigation tile is opaque, so building it first would put
    // these behind it.
    lv_obj_t *settings_btn = lv_btn_create(parent);
    // The touch target is bigger than the button.
    //
    // 34x24 is about 6mm by 4mm on this panel, which is smaller than the
    // fingertip aiming at it, and it sits in the very corner where a finger
    // cannot be centred on it at all. lv_obj_set_ext_click_area grows the area
    // that responds without growing the thing that is drawn, so the control
    // stays the size the layout wants and stops being a game of accuracy.
    lv_obj_set_size(settings_btn, 34, 24);
    lv_obj_set_ext_click_area(settings_btn, 12);
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

    // The time and its zone, in a flex row rather than aligned to each other.
    //
    // The caption used to be placed with lv_obj_align_to against the clock,
    // which resolves once, against the clock's width at that instant -- and at
    // that instant the clock read "--:--". When a real time arrived the label
    // changed width and the caption stayed put, so "18:43" and "EDT" ran into
    // each other. A flex row is re-laid out whenever either label resizes, and
    // the gap is a property of the row rather than a number measured once.
    lv_obj_t *clock_row = lv_obj_create(parent);
    lv_obj_remove_style_all(clock_row);
    lv_obj_set_size(clock_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(clock_row, LV_OBJ_FLAG_SCROLLABLE);
    // Not clickable: lv_obj_create sets that flag and this is a container,
    // not a control. A transparent box that answers touches is how the road
    // layer once swallowed every tap meant for the map beneath it.
    lv_obj_clear_flag(clock_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(clock_row, LV_FLEX_FLOW_ROW);
    // Bottom-aligned, so the small caption sits on the time's baseline rather
    // than floating at its cap height.
    lv_obj_set_flex_align(clock_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(clock_row, 5, 0);
    lv_obj_align(clock_row, LV_ALIGN_TOP_MID, -8, 6);

    s_clock_label = lv_label_create(clock_row);
    lv_obj_set_style_text_font(s_clock_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_clock_label, lv_color_hex(COLOR_VALUE), 0);
    lv_label_set_text(s_clock_label, "--:--");

    s_clock_caption = lv_label_create(clock_row);
    lv_obj_set_style_text_font(s_clock_caption, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_clock_caption, lv_color_hex(COLOR_CAPTION), 0);
    lv_label_set_text(s_clock_caption, "");
    lv_obj_set_style_pad_bottom(s_clock_caption, 2, 0);

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
    // 40pt, a quarter again on 32 and the largest the cell can hold in either
    // direction. Vertically the 40pt line box is 44px under a 14px caption row
    // in a 60px cell; horizontally "18.5" is about 77px and the ride block
    // starts at 90. 48 would fit neither.
    lv_obj_t *speed_cell = MakeCell(parent, COL1, ROW1, STATS_W, CELL_H, "SPEED");
    s_speed_label = MakeValueIn(speed_cell, "--", COLOR_VALUE, &lv_font_montserrat_40);
    s_speed_unit_label = MakeUnit(speed_cell, Settings_SpeedUnitLabel());
    lv_obj_set_style_text_color(s_speed_unit_label, lv_color_hex(COLOR_ACCENT), 0);
    MakeSecondary(speed_cell, &s_speed_avg_label, &s_speed_max_label);

    lv_obj_t *hr_cell = MakeCell(parent, COL1, ROW2, STATS_W, CELL_H, "HEART RATE");
    s_hr_label = MakeValueIn(hr_cell, "--", COLOR_VALUE, &lv_font_montserrat_40);
    MakeUnit(hr_cell, "bpm");
    MakeSecondary(hr_cell, &s_hr_avg_label, &s_hr_max_label);

    // 28pt against the 32 opposite: near enough that the four cells read as one
    // grid, small enough that "188.4" fits a column narrowed to give the ride
    // averages room to be legible.
    lv_obj_t *trip_cell = MakeCell(parent, COL2, ROW1, SEC_W, CELL_H, "TRIP");
    s_trip_label = MakeValueIn(trip_cell, "0.00", COLOR_VALUE, &lv_font_montserrat_28);
    s_trip_unit_label = MakeUnit(trip_cell, Settings_DistanceUnitLabel());

    // Starts as ASCENT and becomes INCLINE if an IMU ever speaks up.
    s_incline_cell =
        MakeCell(parent, COL2, ROW2, SEC_W, CELL_H, "ASCENT", &s_incline_caption);
    s_incline_label = MakeValueIn(s_incline_cell, "0", COLOR_ACCENT, &lv_font_montserrat_28);
    s_incline_unit_label = MakeUnit(s_incline_cell, "m");

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
    s_speed_avg_label = nullptr;
    s_speed_max_label = nullptr;
    s_hr_avg_label = nullptr;
    s_hr_max_label = nullptr;
    s_route_arrow_label = nullptr;
    s_route_dir_label = nullptr;
    s_nav_content = nullptr;
    s_route_dist_row = nullptr;
    s_route_dist_label = nullptr;
    s_route_dist_unit = nullptr;
    s_route_bar = nullptr;
    s_route_exit_label = nullptr;
    s_route_secondary_row = nullptr;
    s_route_then_label = nullptr;
    s_route_remaining_label = nullptr;
    s_trip_unit_label = nullptr;
    s_incline_cell = nullptr;
    s_incline_caption = nullptr;
    s_incline_unit_label = nullptr;
    s_nav_cell = nullptr;
    s_nav_is_map = false;
    for (int i = 0; i < HR_ZONE_COUNT; i++) {
        s_zone_segments[i] = nullptr;
    }
    s_zone_marker = nullptr;
    s_zone_bar_w = 0;
    s_battery_label = nullptr;
}
