#include "RoadView.h"

#include <Arduino.h>

#include "../navigation/MapProject.h"
#include "../navigation/RoadMap.h"

namespace {

volatile uint32_t g_draw_us = 0;
volatile uint32_t g_segments = 0;

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

    g_draw_us = micros() - started;
    g_segments = segments;
}

} // namespace

void RoadView_Attach(MapView_t *view) {
    if (view == nullptr || view->container == nullptr || !RoadMap_IsLoaded()) {
        return;
    }

    lv_obj_t *layer = lv_obj_create(view->container);
    lv_obj_set_pos(layer, 0, 0);
    lv_obj_set_size(layer, view->width, view->height);
    lv_obj_set_style_bg_opa(layer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(layer, 0, 0);
    lv_obj_set_style_pad_all(layer, 0, 0);
    lv_obj_clear_flag(layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(layer, RoadDrawCb, LV_EVENT_DRAW_MAIN, nullptr);

    // Behind the trail, in front of the container's background. Index 0 is the
    // back of the child list, and MapView creates the trail before this runs.
    lv_obj_move_to_index(layer, 0);
}

uint32_t RoadView_LastDrawUs() {
    return g_draw_us;
}

uint32_t RoadView_LastSegments() {
    return g_segments;
}
