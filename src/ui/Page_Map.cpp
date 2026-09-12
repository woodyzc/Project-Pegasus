#include "Page_Map.h"

#include <Arduino.h>
#include <stdio.h>

#include "../hal/LvglFs.h"

#include "../navigation/GpxTrack.h"
#include "../navigation/MapProject.h"
#include "../navigation/RoadMap.h"
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

// The one tile the spike draws.
//
// Under /MAP rather than loose at the card root. That is where tilegen.py
// writes them and where the first bring-up actually put them -- the firmware
// was looking at the root and reported "not found", which is the correct
// answer to the wrong question. A prefix is better anyway: thousands of zoom
// directories scattered beside a rider's .gpx files is not a filesystem
// anyone wants to look at.
#define TILE_Z 15
#define TILE_X 8721
#define TILE_Y 12556
#define TILE_SPIKE_PATH "/MAP/15/8721/12556.bin"

// Half a tile, so both seams of the 2x2 fall inside the window rather than
// off its edges where they prove nothing.
#define TILE_OFF_X 120
#define TILE_OFF_Y 100

// ---- Vector roads ----
// One object with a draw callback, not one lv_line per road. A city has
// thousands of ways, and thousands of lv_obj would cost memory and a layout
// pass each; this draws them all in a single callback with no objects at all.
lv_obj_t *s_road_layer = nullptr;
volatile uint32_t s_road_draw_us = 0;
volatile uint32_t s_road_segments = 0;

struct RoadStyle {
    uint32_t colour;
    lv_coord_t width;
};

// Widths in screen pixels, chosen for a 240px panel rather than scaled from
// the data: a residential street and a trunk road have to be told apart at
// arm's length, which is a display question, not a cartographic one.
const RoadStyle ROAD_STYLE[ROAD_CLASS_COUNT] = {
    {0x333A42, 1}, // minor
    {0x4E5760, 2}, // secondary
    {0xC8A050, 3}, // artery
    {0x1C3E5C, 4}, // water
};

void RoadDrawCb(lv_event_t *e) {
    if (!RoadMap_IsLoaded()) {
        return;
    }
    lv_obj_t *obj = lv_event_get_target(e);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);

    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    const lv_coord_t w = lv_area_get_width(&area);
    const lv_coord_t h = lv_area_get_height(&area);

    double min_lat, min_lon, max_lat, max_lon;
    RoadMap_Bounds(&min_lat, &min_lon, &max_lat, &max_lon);
    const double mpp = Map_FitScale(min_lat, max_lat, min_lon, max_lon, w, h, 4);
    const double clat = (min_lat + max_lat) / 2.0;
    const double clon = (min_lon + max_lon) / 2.0;

    const uint32_t started = micros();
    uint32_t segments = 0;

    // Water first, then minor, then secondary, then arteries: painter's order,
    // so a trunk road crosses a river rather than being cut by it.
    for (int pass = ROAD_CLASS_COUNT - 1; pass >= 0; pass--) {
        const int klass = (pass == ROAD_CLASS_COUNT - 1) ? ROAD_CLASS_WATER : pass;
        if (pass != ROAD_CLASS_COUNT - 1 && klass == ROAD_CLASS_WATER) {
            continue;
        }

        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.color = lv_color_hex(ROAD_STYLE[klass].colour);
        dsc.width = ROAD_STYLE[klass].width;
        dsc.round_start = 1;
        dsc.round_end = 1;

        for (size_t i = 0; i < RoadMap_WayCount(); i++) {
            RoadWay_t way;
            if (!RoadMap_Way(i, &way) || way.klass != klass || way.count < 2) {
                continue;
            }
            lv_point_t prev;
            for (uint16_t k = 0; k < way.count; k++) {
                int16_t x, y;
                Map_Project(way.points[k * 2] / ROADMAP_COORD_SCALE,
                            way.points[k * 2 + 1] / ROADMAP_COORD_SCALE, clat, clon, mpp,
                            (int16_t)(w / 2), (int16_t)(h / 2), &x, &y);
                lv_point_t p = {(lv_coord_t)(area.x1 + x), (lv_coord_t)(area.y1 + y)};
                if (k > 0) {
                    lv_draw_line(ctx, &dsc, &prev, &p);
                    segments++;
                }
                prev = p;
            }
        }
    }

    s_road_draw_us = micros() - started;
    s_road_segments = segments;
}

lv_obj_t *s_tile_layer = nullptr;
lv_obj_t *s_tile_img = nullptr;
lv_obj_t *s_tile_stat = nullptr;
lv_timer_t *s_tile_timer = nullptr;

// Reports what the tile cost, or why there wasn't one. The failure case names
// the path it tried: "not found" on its own sent the first bring-up looking in
// the wrong place, when the answer was one directory away.
void TileStatTimer(lv_timer_t *timer) {
    (void)timer;
    if (s_tile_stat == nullptr) {
        return;
    }

    if (!LvglFs_IsReady()) {
        lv_label_set_text(s_tile_stat, "tile: no fs driver (card not mounted?)");
        return;
    }

    const uint32_t bytes = LvglFs_LastReadBytes();
    if (bytes >= 4096) {
        const uint32_t us = LvglFs_LastReadUs();
        lv_label_set_text_fmt(s_tile_stat, "tile %u B in %u ms = %.2f MB/s",
                              (unsigned)bytes, (unsigned)(us / 1000),
                              us > 0 ? (double)bytes / (double)us : 0.0);
        return;
    }

    if (RoadMap_IsLoaded()) {
        lv_label_set_text_fmt(s_tile_stat, "roads %u B, %u ways, %u seg in %u us",
                              (unsigned)RoadMap_Bytes(), (unsigned)RoadMap_WayCount(),
                              (unsigned)s_road_segments, (unsigned)s_road_draw_us);
        return;
    }

    if (LvglFs_OpenFailures() > 0) {
        // Ask the card what it does have, rather than leaving the reader to
        // guess which component of the path is wrong.
        char probe[96];
        LvglFs_Probe(TILE_SPIKE_PATH, probe, sizeof(probe));
        lv_label_set_text_fmt(s_tile_stat, "no %s\n%s", LvglFs_LastFailedPath(), probe);
        return;
    }
    lv_label_set_text(s_tile_stat, "tile: header only, not drawn yet");
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
        // A clipping container, so tiles can hang off the edges. LVGL clips
        // children to their parent, which is the only way a tile can start at
        // a negative offset without painting over the ROUTE title.
        s_tile_layer = lv_obj_create(parent);
        lv_obj_set_pos(s_tile_layer, MAP_X, MAP_Y);
        lv_obj_set_size(s_tile_layer, MAP_W, MAP_H);
        lv_obj_set_style_bg_opa(s_tile_layer, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_tile_layer, 0, 0);
        lv_obj_set_style_pad_all(s_tile_layer, 0, 0);
        lv_obj_clear_flag(s_tile_layer, LV_OBJ_FLAG_SCROLLABLE);

        // 2x2, deliberately offset.
        //
        // One 256px tile all but fills a 240x262 window -- 0.94 by 1.02 of it
        // -- so a single tile shows nothing about whether neighbours line up.
        // Offsetting by roughly half a tile puts both seams on screen, which
        // is the only thing worth checking at this stage: a road that jumps
        // at a seam means the projection is wrong, and that is the failure
        // step 3 exists to avoid.
        for (int tx = 0; tx < 2; tx++) {
            for (int ty = 0; ty < 2; ty++) {
                lv_obj_t *img = lv_img_create(s_tile_layer);
                char path[64];
                snprintf(path, sizeof(path), "S:/MAP/%d/%d/%d.bin", TILE_Z,
                         TILE_X + tx, TILE_Y + ty);
                lv_img_set_src(img, path);
                lv_obj_set_pos(img, tx * 256 - TILE_OFF_X, ty * 256 - TILE_OFF_Y);
                if (tx == 0 && ty == 0) {
                    s_tile_img = img;
                }
            }
        }
    }

    // Roads under the track, over the tiles.
    if (RoadMap_IsLoaded()) {
        s_road_layer = lv_obj_create(parent);
        lv_obj_set_pos(s_road_layer, MAP_X, MAP_Y);
        lv_obj_set_size(s_road_layer, MAP_W, MAP_H);
        lv_obj_set_style_bg_opa(s_road_layer, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_road_layer, 0, 0);
        lv_obj_clear_flag(s_road_layer, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(s_road_layer, RoadDrawCb, LV_EVENT_DRAW_MAIN, nullptr);
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
    lv_obj_set_width(s_tile_stat, 224);
    lv_label_set_long_mode(s_tile_stat, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_tile_stat, LV_ALIGN_TOP_LEFT, 8, 26);
    lv_label_set_text(s_tile_stat, "tile: waiting");

    // On a timer, not once here.
    //
    // lv_img_set_src() reads only the 4-byte header -- LVGL defers the pixels
    // to draw time, which has not happened yet when this page is being built.
    // Sampling now reported 4 bytes at best and nothing at worst, and never
    // the tile. The first tick after the first draw is when the real figure
    // exists.
    s_tile_timer = lv_timer_create(TileStatTimer, 500, nullptr);
    TileStatTimer(nullptr);

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
    // Same reasoning as the refresh timer above: it outlives the widgets
    // unless torn down here, and would write to freed lv_obj pointers on its
    // next tick.
    if (s_tile_timer != nullptr) {
        lv_timer_del(s_tile_timer);
        s_tile_timer = nullptr;
    }
    DataCenter_Unsubscribe(TOPIC_GPS_INFO, &s_gps_account);

    s_status_label = nullptr;
    s_scale_label = nullptr;
    s_road_layer = nullptr;
    s_tile_layer = nullptr;
    s_tile_img = nullptr;
    s_tile_stat = nullptr;
}
