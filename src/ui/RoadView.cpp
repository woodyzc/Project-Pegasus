#include "RoadView.h"

#include <Arduino.h>
#include <math.h>

#include "../navigation/MapProject.h"
#include "../navigation/RoadMap.h"

namespace {

volatile uint32_t g_draw_us = 0;
volatile uint32_t g_segments = 0;
volatile uint32_t g_visible = 0;

struct RoadStyle {
    uint32_t colour;
    lv_coord_t width;
};

// Widths in screen pixels, chosen for a 240px panel rather than scaled from
// the data: a residential street and a trunk road have to be told apart at
// arm's length, which is a display question, not a cartographic one.
// Above this many metres per pixel, a class stops being drawn.
//
// Real data forced this. 20874 is 3,560 ways and 45,413 points; drawing all of
// it at ~40us a segment is 1.8 seconds, which is not a frame, it is a pause.
// Every map renderer solves it the same way -- show less when zoomed out --
// and the thresholds are about legibility as much as speed: residential
// streets at 4km across are a grey smear that hides the road you want.
//
// 8 mpp is ~1.9km across this panel, 20 is ~4.8km.
const double ROAD_MAX_MPP[ROAD_CLASS_COUNT] = {
    8.0,    // minor      -- only when close in
    20.0,   // secondary
    1e9,    // artery     -- always; it is the thing you navigate by
    1e9,    // water      -- always; the strongest landmark on a small screen
};

// A ceiling on one frame regardless of what the card holds. The zoom filter
// bounds things for sane data, this bounds them for data nobody anticipated --
// a dense city centre, or someone's continent-wide export. 4000 segments is
// ~160ms, visibly a redraw but not a hang.
constexpr uint32_t ROAD_MAX_SEGMENTS = 4000;

// Ways that can be on screen at once. Static rather than on the stack: this
// runs on the LVGL task, whose stack is 8KB, and 2048 entries is 4KB of it.
constexpr uint16_t ROAD_VISIBLE_MAX = 2048;

// Manhattan distance below which a point is folded into the previous one.
// 3px keeps curves smooth at this screen size while collapsing the runs of
// near-identical points OSM records along a straight road.
constexpr int ROAD_MIN_SEGMENT_PX = 3;

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

    // Cull ONCE, then draw from what survived.
    //
    // The first version tested every way again on every class pass -- four
    // times 3,560 ways for the Germantown extract, and 7.7ms measured to draw
    // precisely nothing when the view was elsewhere. The bounds test is cheap;
    // doing it four times over the whole file is not.
    static uint16_t visible[ROAD_VISIBLE_MAX];
    uint16_t visible_count = 0;

    for (size_t i = 0; i < ways && visible_count < ROAD_VISIBLE_MAX; i++) {
        RoadWay_t way;
        if (!RoadMap_Way(i, &way) || way.count < 2) {
            continue;
        }
        if (mpp > ROAD_MAX_MPP[way.klass]) {
            continue; // too far out for this class to be legible
        }
        if (way.max_lat < view_min_lat || way.min_lat > view_max_lat ||
            way.max_lon < view_min_lon || way.min_lon > view_max_lon) {
            continue;
        }
        visible[visible_count++] = (uint16_t)i;
    }

    // Water, then minor, then secondary, then arteries -- painter's order, so
    // a trunk road crosses a river rather than being cut by it.
    static const uint8_t ORDER[ROAD_CLASS_COUNT] = {
        ROAD_CLASS_WATER, ROAD_CLASS_MINOR, ROAD_CLASS_SECONDARY, ROAD_CLASS_ARTERY};

    for (int pass = 0; pass < ROAD_CLASS_COUNT && segments < ROAD_MAX_SEGMENTS; pass++) {
        const uint8_t klass = ORDER[pass];

        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.color = lv_color_hex(ROAD_STYLE[klass].colour);
        dsc.width = ROAD_STYLE[klass].width;
        dsc.round_start = 1;
        dsc.round_end = 1;

        for (uint16_t v = 0; v < visible_count && segments < ROAD_MAX_SEGMENTS; v++) {
            RoadWay_t way;
            if (!RoadMap_Way(visible[v], &way) || way.klass != klass) {
                continue;
            }
            // Decimate while projecting: a point that lands within a pixel or
            // two of the last one drawn cannot change what appears, and
            // drawing it costs the same as one that can.
            //
            // This is where the frame time was going. OSM records geometry at
            // metre resolution; at 40 metres a pixel that is dozens of points
            // per pixel, and 4,000 segments took 128ms to produce a line no
            // different from the one 400 would have drawn. MapProject already
            // does exactly this for the recorded track -- see
            // Map_BuildPolyline -- and the roads simply were not.
            lv_point_t prev = {0, 0};
            bool have_prev = false;
            for (uint16_t k = 0; k < way.count; k++) {
                int16_t x, y;
                Map_Project(way.points[k * 2] / ROADMAP_COORD_SCALE,
                            way.points[k * 2 + 1] / ROADMAP_COORD_SCALE, clat, clon, mpp,
                            (int16_t)(w / 2), (int16_t)(h / 2), &x, &y);
                lv_point_t p = {(lv_coord_t)(area.x1 + x), (lv_coord_t)(area.y1 + y)};

                if (have_prev) {
                    const int dx = p.x > prev.x ? p.x - prev.x : prev.x - p.x;
                    const int dy = p.y > prev.y ? p.y - prev.y : prev.y - p.y;
                    // Always draw the final point, or a way shorter than the
                    // threshold would vanish entirely rather than simplify.
                    if (dx + dy < ROAD_MIN_SEGMENT_PX && k + 1 < way.count) {
                        continue;
                    }
                    lv_draw_line(ctx, &dsc, &prev, &p);
                    segments++;
                }
                prev = p;
                have_prev = true;
            }
        }
    }

    g_visible = visible_count;
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

uint32_t RoadView_LastVisibleWays() {
    return g_visible;
}
