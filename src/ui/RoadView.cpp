#include "RoadView.h"

#include <Arduino.h>
#include <math.h>

#include "../navigation/MapProject.h"
#include "../navigation/RoadMap.h"

namespace {

volatile uint32_t g_draw_us = 0;
volatile uint32_t g_segments = 0;
volatile uint32_t g_visible = 0;
volatile uint32_t g_cull_us = 0;
volatile uint32_t g_draw_only_us = 0;

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

// A ceiling on one frame regardless of what the card holds.
//
// 1400, not the 4000 this was. A segment measured ~51us on the panel -- 886 of
// them in a 45ms draw -- so 4000 is over 200ms, which is not "visibly a
// redraw", it is the board feeling broken. A dense extract reaches that
// ceiling on every frame, which is what made the Arlington map unusable while
// Germantown was fine: same way count, three times the arteries.
constexpr uint32_t ROAD_MAX_SEGMENTS = 1400;

// And a share per class, because the ceiling alone starves the wrong ones.
// The passes run in painter's order -- water under roads -- so a global budget
// spent by the time the artery pass runs leaves the map without the roads a
// rider actually navigates by. Indexed by ROAD_CLASS_*.
const uint32_t ROAD_CLASS_SEGMENTS[ROAD_CLASS_COUNT] = {
    400,  // minor
    400,  // secondary
    900,  // artery  -- the most, and drawn last, so it needs protecting
    400,  // water
};

// Ways that can be on screen at once. Static rather than on the stack: this
// runs on the LVGL task, whose stack is 8KB, and 2048 entries is 4KB of it.
// Sized to what the segment budget can actually draw, not to what is in view.
//
// This was briefly 6144, on the reasoning that a dense extract has that many
// arteries on screen. It does, and drawing them is what made the board crawl:
// the budget above stops at 1400 segments, so thousands of candidates are
// walked and then abandoned -- and because the list arrives in grid order, the
// budget is spent on whichever corner came first, which is the same bias the
// query was just fixed for, moved one stage later.
//
// A thousand candidates, evenly sampled by the query, is close to what 1400
// segments covers. Nearly all of them get drawn, so the thinning is the even
// one the query chose rather than an arbitrary cut-off. 1024 entries is 4KB
// each for the list and its sorted copy.
constexpr uint16_t ROAD_VISIBLE_MAX = 1024;

// Manhattan distance below which a point is folded into the previous one.
// 3px keeps curves smooth at this screen size while collapsing the runs of
// near-identical points OSM records along a straight road.
constexpr int ROAD_MIN_SEGMENT_PX = 3;

// Region codes for rejecting a segment that cannot cross the view.
//
// Culling is per WAY, and a way only has to touch the view to survive it --
// then every one of its points is drawn, including the miles of it that are
// nowhere near the screen. At 480m across that was 966 segments from 55 ways
// and a 38ms frame, most of it spent drawing road that was never visible.
//
// Two endpoints sharing any outside edge cannot have the segment between them
// cross the view, which is the cheap half of Cohen-Sutherland and all that is
// needed here: LVGL clips the drawing correctly either way, this just avoids
// asking it to.
enum { OUT_LEFT = 1, OUT_RIGHT = 2, OUT_TOP = 4, OUT_BOTTOM = 8 };

inline uint8_t OutCode(const lv_point_t *p, const lv_area_t *a) {
    uint8_t code = 0;
    if (p->x < a->x1) code |= OUT_LEFT;
    else if (p->x > a->x2) code |= OUT_RIGHT;
    if (p->y < a->y1) code |= OUT_TOP;
    else if (p->y > a->y2) code |= OUT_BOTTOM;
    return code;
}

const RoadStyle ROAD_STYLE[ROAD_CLASS_COUNT] = {
    {0x333A42, 1}, // minor
    {0x4E5760, 2}, // secondary
    // Arteries in blue, not the amber they were. Amber is the trail's own
    // colour family now that the ridden part of it is yellow, and a road
    // sharing that family is a road a rider mistakes for the route.
    //
    // Muted rather than the bright blue this first was. Roads are the backdrop
    // the route is read against, and a bright artery pulled the eye off the
    // one line on the map that matters. Still a step lighter than the water
    // below it, which is darker again and drawn thicker.
    {0x2F6389, 3}, // artery
    {0x1C3E5C, 4}, // water
};

// The MapView this layer belongs to. Its projection is the one that matters:
// roads and the recorded track have to be drawn through the SAME centre and
// scale or they are two maps of the same place that do not agree, and a trail
// sitting beside the road it was recorded on is worse than no road at all.
// Every attached layer, so a redraw reaches all of them.
//
// There used to be one global MapView_t here and the draw callback read it,
// which was wrong the moment two layers existed at once. The dashboard's map
// and the ROUTE page's are both attached -- the dashboard is a cached page and
// its layer outlives a visit to the route page -- so both drew through
// whichever view had attached last. Going back to the dashboard left its trail
// drawn at its own scale over roads drawn at the route page's, and the two
// maps of the same place disagreed by exactly the difference in their heights.
//
// Each layer now carries its own view in its user data. The array is only so
// that Refresh can invalidate them all; nothing here is a global camera.
constexpr int MAX_LAYERS = 4;
lv_obj_t *g_layers[MAX_LAYERS] = {nullptr};

// Kept for the statistics below, which describe the last draw whoever made it.
lv_obj_t *g_layer = nullptr;

void ForgetLayer(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    for (int i = 0; i < MAX_LAYERS; i++) {
        if (g_layers[i] == obj) {
            g_layers[i] = nullptr;
        }
    }
    if (g_layer == obj) {
        g_layer = nullptr;
    }
}

void RoadDrawCb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    MapView_t *g_view = (MapView_t *)lv_obj_get_user_data(obj);
    if (!RoadMap_IsLoaded() || g_view == nullptr) {
        return;
    }
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

    // Prepared once for the whole frame. The per-point path is then two
    // multiplies and two adds, where Map_Project would recompute a cosine and
    // two divisions for every point of every visible way.
    MapProjection_t proj;
    Map_PrepareProjection(&proj, clat, clon, mpp, (int16_t)(w / 2), (int16_t)(h / 2));

    // Timed in two halves, because three rounds of optimising the drawing have
    // each moved the number less than expected. Guessing which half is
    // expensive has a poor record here; the split says outright.
    const uint32_t started = micros();
    uint32_t segments = 0;

    // Ask the index once, then draw from what it returned.
    //
    // The grid answers "which ways are near here" without touching the rest
    // of the file. Scanning every way cost 9,904us of a 12,272us frame to find
    // 23 of 7,964 -- not arithmetic, but 7,964 scattered PSRAM reads.
    // Which classes this zoom will actually draw, decided BEFORE the query
    // rather than after it. Asking for ways that are about to be discarded is
    // what filled the buffer with residential streets at 9km across and left
    // no room for the arteries the rider navigates by.
    uint8_t class_mask = 0;
    for (int k = 0; k < ROAD_CLASS_COUNT; k++) {
        if (mpp <= ROAD_MAX_MPP[k]) {
            class_mask |= (uint8_t)(1u << k);
        }
    }

    // Static, not on the stack: this runs on the LVGL task, whose stack is 8KB.
    static uint32_t visible[ROAD_VISIBLE_MAX];
    static uint8_t visible_class[ROAD_VISIBLE_MAX];
    static uint32_t sorted[ROAD_VISIBLE_MAX];

    uint16_t visible_count = (uint16_t)RoadMap_Query(view_min_lat, view_min_lon, view_max_lat,
                                                     view_max_lon, class_mask, visible,
                                                     ROAD_VISIBLE_MAX);

    // Each way's record is read ONCE here, and the list is sorted into the
    // order the passes below want it.
    //
    // The passes used to walk the whole list four times, calling RoadMap_Way
    // on every entry to ask its class -- four scattered PSRAM reads per way
    // per frame, which is the access pattern the spatial grid exists to avoid.
    // At 6144 candidates that is 24,576 of them, and they are not cheap: the
    // measurement that justified the grid was 7,964 such reads costing 9.9ms.
    uint16_t kept = 0;
    uint16_t class_count[ROAD_CLASS_COUNT] = {0};
    for (uint16_t i = 0; i < visible_count; i++) {
        RoadWay_t way;
        if (!RoadMap_Way(visible[i], &way) || way.count < 2) {
            continue;
        }
        visible[kept] = visible[i];
        visible_class[kept] = way.klass;
        class_count[way.klass]++;
        kept++;
    }
    visible_count = kept;

    g_cull_us = micros() - started;
    const uint32_t draw_started = micros();

    // Water, then minor, then secondary, then arteries -- painter's order, so
    // a trunk road crosses a river rather than being cut by it.
    static const uint8_t ORDER[ROAD_CLASS_COUNT] = {
        ROAD_CLASS_WATER, ROAD_CLASS_MINOR, ROAD_CLASS_SECONDARY, ROAD_CLASS_ARTERY};

    // Counting sort into painter's order, so each pass walks a contiguous slice
    // of its own class instead of the whole list.
    uint16_t slice_start[ROAD_CLASS_COUNT] = {0};
    {
        uint16_t at = 0;
        uint16_t cursor[ROAD_CLASS_COUNT];
        for (int pass = 0; pass < ROAD_CLASS_COUNT; pass++) {
            const uint8_t klass = ORDER[pass];
            slice_start[klass] = at;
            cursor[klass] = at;
            at = (uint16_t)(at + class_count[klass]);
        }
        for (uint16_t i = 0; i < visible_count; i++) {
            sorted[cursor[visible_class[i]]++] = visible[i];
        }
    }

    for (int pass = 0; pass < ROAD_CLASS_COUNT && segments < ROAD_MAX_SEGMENTS; pass++) {
        const uint8_t klass = ORDER[pass];
        const uint16_t from = slice_start[klass];
        const uint16_t to = (uint16_t)(from + class_count[klass]);
        uint32_t class_segments = 0;

        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.color = lv_color_hex(ROAD_STYLE[klass].colour);
        dsc.width = ROAD_STYLE[klass].width;
        dsc.round_start = 1;
        dsc.round_end = 1;

        for (uint16_t v = from; v < to && segments < ROAD_MAX_SEGMENTS &&
                                class_segments < ROAD_CLASS_SEGMENTS[klass];
             v++) {
            RoadWay_t way;
            if (!RoadMap_Way(sorted[v], &way)) {
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
            uint8_t prev_code = 0;
            bool have_prev = false;
            for (uint16_t k = 0; k < way.count; k++) {
                int16_t x, y;
                Map_ProjectPrepared(&proj, way.points[k * 2] / ROADMAP_COORD_SCALE,
                                    way.points[k * 2 + 1] / ROADMAP_COORD_SCALE, &x, &y);
                lv_point_t p = {(lv_coord_t)(area.x1 + x), (lv_coord_t)(area.y1 + y)};

                const uint8_t code = OutCode(&p, &area);

                if (have_prev) {
                    const int dx = p.x > prev.x ? p.x - prev.x : prev.x - p.x;
                    const int dy = p.y > prev.y ? p.y - prev.y : prev.y - p.y;
                    // Always draw the final point, or a way shorter than the
                    // threshold would vanish entirely rather than simplify.
                    if (dx + dy < ROAD_MIN_SEGMENT_PX && k + 1 < way.count) {
                        continue;
                    }
                    // Both ends off the same side: the segment cannot cross
                    // the view, so there is nothing for LVGL to clip.
                    if ((code & prev_code) == 0) {
                        lv_draw_line(ctx, &dsc, &prev, &p);
                        segments++;
                        class_segments++;
                    }
                }
                prev = p;
                prev_code = code;
                have_prev = true;
            }
        }
    }

    g_visible = visible_count;
    g_draw_only_us = micros() - draw_started;
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
    // The view travels with the layer, not in a global, so two layers can be
    // attached at once and each draws through its own projection.
    lv_obj_set_user_data(layer, view);
    g_layer = layer;
    for (int i = 0; i < MAX_LAYERS; i++) {
        if (g_layers[i] == nullptr) {
            g_layers[i] = layer;
            break;
        }
    }
    lv_obj_add_event_cb(layer, RoadDrawCb, LV_EVENT_DRAW_MAIN, nullptr);
    // A page tearing down destroys its layer with its widgets, and a pointer
    // to it must not outlive that.
    lv_obj_add_event_cb(layer, ForgetLayer, LV_EVENT_DELETE, nullptr);

    // Behind the trail, in front of the container's background. Index 0 is the
    // back of the child list, and MapView creates the trail before this runs.
    lv_obj_move_to_index(layer, 0);
}

void RoadView_Refresh() {
    // All of them. A hidden page's layer costs nothing to invalidate and LVGL
    // will not draw it, and the alternative is tracking which page is visible
    // in a module that has no business knowing.
    for (int i = 0; i < MAX_LAYERS; i++) {
        if (g_layers[i] != nullptr) {
            lv_obj_invalidate(g_layers[i]);
        }
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

uint32_t RoadView_LastCullUs() {
    return g_cull_us;
}

uint32_t RoadView_LastDrawOnlyUs() {
    return g_draw_only_us;
}
