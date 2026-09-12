#include "RoadView.h"

#include <Arduino.h>
#include <math.h>

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

// The MapView this layer belongs to. Its projection is the one that matters:
// roads and the recorded track have to be drawn through the SAME centre and
// scale or they are two maps of the same place that do not agree, and a trail
// sitting beside the road it was recorded on is worse than no road at all.
MapView_t *g_view = nullptr;
lv_obj_t *g_layer = nullptr;

void RoadDrawCb(lv_event_t *e) {
    if (!RoadMap_IsLoaded() || g_view == nullptr) {
        return;
    }
    lv_obj_t *obj = lv_event_get_target(e);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);

    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    const lv_coord_t w = lv_area_get_width(&area);
    const lv_coord_t h = lv_area_get_height(&area);

    double clat, clon, mpp;
    if (g_view->have_center) {
        clat = g_view->center_lat;
        clon = g_view->center_lon;
        mpp = g_view->metres_per_pixel;
    } else {
        // Nothing has set a view yet -- no fix, no track. Frame the roads
        // themselves so the map is not simply blank while waiting.
        double min_lat, min_lon, max_lat, max_lon;
        if (!RoadMap_Bounds(&min_lat, &min_lon, &max_lat, &max_lon)) {
            return;
        }
        clat = (min_lat + max_lat) / 2.0;
        clon = (min_lon + max_lon) / 2.0;
        mpp = Map_FitScale(min_lat, max_lat, min_lon, max_lon, w, h, 4);
    }
    if (mpp <= 0.0) {
        return;
    }

    // What the view covers, in degrees, so a way can be rejected without
    // projecting any of it. Generous by a tile's worth on each side: a way
    // whose bounding box is outside still has segments that cross the corner.
    const double half_h_deg = (h * 0.5 * mpp) / MAP_EARTH_METRES_PER_DEGREE * 1.5;
    const double cos_lat = cos(clat * M_PI / 180.0);
    const double half_w_deg =
        (w * 0.5 * mpp) / (MAP_EARTH_METRES_PER_DEGREE * (cos_lat > 0.01 ? cos_lat : 0.01)) * 1.5;
    const int32_t view_min_lat = (int32_t)((clat - half_h_deg) * ROADMAP_COORD_SCALE);
    const int32_t view_max_lat = (int32_t)((clat + half_h_deg) * ROADMAP_COORD_SCALE);
    const int32_t view_min_lon = (int32_t)((clon - half_w_deg) * ROADMAP_COORD_SCALE);
    const int32_t view_max_lon = (int32_t)((clon + half_w_deg) * ROADMAP_COORD_SCALE);

    const uint32_t started = micros();
    uint32_t segments = 0;
    const size_t ways = RoadMap_WayCount();

    // Water, then minor, then secondary, then arteries -- painter's order, so
    // a trunk road crosses a river rather than being cut by it.
    static const uint8_t ORDER[ROAD_CLASS_COUNT] = {
        ROAD_CLASS_WATER, ROAD_CLASS_MINOR, ROAD_CLASS_SECONDARY, ROAD_CLASS_ARTERY};

    for (int pass = 0; pass < ROAD_CLASS_COUNT; pass++) {
        const uint8_t klass = ORDER[pass];

        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.color = lv_color_hex(ROAD_STYLE[klass].colour);
        dsc.width = ROAD_STYLE[klass].width;
        dsc.round_start = 1;
        dsc.round_end = 1;

        for (size_t i = 0; i < ways; i++) {
            RoadWay_t way;
            if (!RoadMap_Way(i, &way) || way.klass != klass || way.count < 2) {
                continue;
            }
            // Four integer comparisons to skip a way entirely. On a real
            // extract nearly every way fails this, which is what keeps the
            // draw bounded by the screen rather than by the file.
            if (way.max_lat < view_min_lat || way.min_lat > view_max_lat ||
                way.max_lon < view_min_lon || way.min_lon > view_max_lon) {
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

    // Not clickable, and this matters more than it looks. lv_obj_create sets
    // LV_OBJ_FLAG_CLICKABLE by default, so this layer -- which covers the
    // whole map -- swallowed every touch and the dashboard's navigation tile
    // stopped opening the ROUTE page. MapView's own trail does not do this
    // because lv_line clears the flag in its constructor.
    //
    // Nothing here is interactive: it is a backdrop, and touches belong to
    // whatever is underneath it.
    lv_obj_clear_flag(layer, LV_OBJ_FLAG_CLICKABLE);
    g_view = view;
    g_layer = layer;
    lv_obj_add_event_cb(layer, RoadDrawCb, LV_EVENT_DRAW_MAIN, nullptr);

    // Behind the trail, in front of the container's background. Index 0 is the
    // back of the child list, and MapView creates the trail before this runs.
    lv_obj_move_to_index(layer, 0);
}

void RoadView_Refresh() {
    if (g_layer != nullptr) {
        lv_obj_invalidate(g_layer);
    }
}

uint32_t RoadView_LastDrawUs() {
    return g_draw_us;
}

uint32_t RoadView_LastSegments() {
    return g_segments;
}
