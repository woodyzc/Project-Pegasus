#include "RoadView.h"

#include <Arduino.h>
#include <math.h>

#include "../hal/Display.h"
#include "../navigation/MapProject.h"
#include "../navigation/RoadMap.h"

// Map_ProjectE7Prepared takes 1e-7 degrees, and the road blob's raw integers
// go straight into it. A different scale in the file format would project
// every road to the wrong place without a single error, so it is checked here.
static_assert(ROADMAP_COORD_SCALE == 1e7, "road points must be 1e-7 degrees");

namespace {

// Figures for the settings page; RoadView.h says what each one answers. All of
// them describe a BUILD (one projection of the view) or the frame drawn from
// it, never a single strip -- see the note above RoadDrawCb for the
// difference, which the first version of these figures did not know about.
volatile uint32_t g_visible = 0;
volatile uint32_t g_segments = 0;
volatile uint32_t g_cull_us = 0;
volatile uint32_t g_build_us = 0;
volatile uint32_t g_frame_draw_us = 0;
volatile uint16_t g_frame_strips = 0;

// ⚠️ Kept separately, because the obvious way to read it does not work.
//
// The settings row shows the LAST draw -- and letting go of the map triggers
// a full-detail repaint, so by the time the rider has navigated to Settings
// the drag figure has already been overwritten by the one it was supposed to
// be compared against. Asking someone to "watch the number while dragging"
// is asking them to look at two pages at once.
//
// So the drag draws record here and stay put until the next drag.
volatile uint32_t g_build_us_drag = 0;
volatile uint32_t g_frame_draw_us_drag = 0;
volatile uint16_t g_segments_drag = 0;

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
//
// 900, not 1400, because widening the roads changed what a segment COSTS and
// this number is a frame-time ceiling denominated in segments. 1400 was
// derived from a measured ~51us; the panel now reports 88ms of draw for 532
// segments, so the calibration behind 1400 no longer describes anything. The
// constant did not become wrong on its own -- it was invalidated one file
// away, by a change that never touched it.
//
// 900 is a BACKSTOP, and measurement says it is nothing more. Three views on
// the panel: 532 segments at 165us each (88ms, close in), 460 at 89us (41ms,
// 1.9km across), both far under this cap. Nothing observed comes near it.
//
// It was set expecting count x cost to multiply freely, and they do not --
// the two are anti-correlated, which is the useful thing learnt here. Long
// expensive segments only occur zoomed IN, where decimation leaves one
// screen-crossing line per way and few ways are in view; high segment counts
// only occur zoomed OUT, where every segment is short. The product is
// self-limiting, so frame cost is really bounded by ink -- screen area times
// overdraw -- which is why views as different as those two land in the same
// 41-88ms band.
//
// Do not read a frame-time ceiling off this number, then. If a frame ever
// does get slow, the thing to measure is coverage, not count; and density is
// a property of WHERE the rider is, not of the zoom -- the 1.9km view above
// is river valley and parkland and holds half the ways of a town centre at
// a quarter the scale.
constexpr uint32_t ROAD_MAX_SEGMENTS = 900;

// And a share per class, because the ceiling alone starves the wrong ones.
// The passes run in painter's order -- water under roads -- so a global budget
// spent by the time the artery pass runs leaves the map without the roads a
// rider actually navigates by. Indexed by ROAD_CLASS_*.
//
// Scaled with the ceiling above rather than left alone: these oversubscribe it
// by half (1360 against 900, as 2100 did against 1400) so a class can use
// another's slack, and holding them fixed while the ceiling fell would have
// quietly raised minor streets' share of a smaller budget -- starving the
// arteries this table exists to protect.
const uint32_t ROAD_CLASS_SEGMENTS[ROAD_CLASS_COUNT] = {
    260,  // minor
    260,  // secondary
    580,  // artery  -- the most, and drawn last, so it needs protecting
    260,  // water
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

// ---- Detail while the finger is down ----
//
// Panning calls RoadView_Refresh on every LV_EVENT_PRESSING, so the road
// layer is redrawn once per frame for as long as the drag lasts. At the
// figures this board reports -- 587 segments in 55ms -- that alone caps
// panning at about 18fps however fast the events arrive, which is what "not
// very smooth" is made of.
//
// The segments are not the expensive part; lv_draw_line is, at roughly 94us
// each with the rounded caps these use. So the lever is to draw FEWER of
// them while the view is moving, and all of them again the moment it stops.
//
// This is what every map application does and it is honest about what it
// trades: detail the rider cannot read anyway during a drag, for a map that
// keeps up with their finger. Nothing is lost when they let go.
constexpr int ROAD_DRAG_MIN_SEGMENT_PX = 9;
constexpr uint32_t ROAD_DRAG_MAX_SEGMENTS = 260;

bool g_interactive = false;

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

// Colours are set against COLOR_MAP_BG (0x0B1116), and they are set by
// contrast ratio rather than by eye, because "looks fine on the bench" is a
// dim room at 30cm and the panel is read in sunlight at arm's length.
//
// What was here before was too dark to see. Minor streets at 0x333A42 are
// 1.65:1 against that background and water at 0x1C3E5C is 1.73:1 -- below the
// 3:1 floor for any graphical object, so on a transflective panel outdoors
// they were a texture rather than a map. The ramp now runs 4.0 / 5.2 / 5.7 /
// 3.0, which keeps the class hierarchy legible as brightness while leaving
// every road well under the trail: COLOR_TRAIL_AHEAD is ~11:1, so the one
// line that is not scenery still wins the eye outright.
//
// Widths are up one step across the board for the same reason. A 1px road on
// a 240px panel is a hairline that anti-aliasing then halves the contrast of
// again -- the two faults compound, which is why the fix has to be both.
//
// Arteries stay blue and stay the lightest road, because they are the thing
// you navigate by; the earlier note about not using amber still holds, since
// amber is the ridden trail's colour. Water is deliberately the most
// saturated and the darkest of the four despite being the widest: it is a
// landmark to recognise, not a route to follow.
// Ratios below are measured against COLOR_MAP_BG, not estimated. Secondary
// was 0x8D98A5 and carried this same "5.2:1" comment while actually
// measuring 6.48:1 -- lighter than the artery it is supposed to sit under,
// which inverted the one part of the hierarchy that matters most. The other
// three were derived correctly (3.93 / 5.68 / 2.92 against their stated
// 4.0 / 5.7 / 3.0); only this entry's hex did not match its own note.
const RoadStyle ROAD_STYLE[ROAD_CLASS_COUNT] = {
    {0x68737F, 2}, // minor      -- 3.93:1
    {0x7D8794, 3}, // secondary  -- 5.21:1
    {0x4D93C4, 4}, // artery     -- 5.68:1, and the lightest road, as intended
    // 4px, not 5. Water was the widest thing on the map and at this colour it
    // became the most dominant feature on screen -- competing with the route,
    // which is the one line that must win outright. Narrowed rather than
    // darkened: it is still the strongest landmark, just no longer heavier
    // than the thing the rider is following.
    {0x27628F, 4}, // water      -- 2.92:1
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

// ---- One projection per frame, however many strips LVGL draws it in ----
//
// ⚠️ LVGL renders into a 40-line buffer (Display.cpp), so a frame of this layer
// is drawn as a stack of horizontal strips, and DRAW_MAIN fires once PER STRIP:
// seven times for the ROUTE page's map, five for the dashboard tile.
//
// Everything above lv_draw_line -- the grid query, reading every way,
// decimating and projecting every point -- depends on the view and not on the
// strip, and it used to run in full for each one. A full repaint paid for the
// projection seven times over, and the figures on the settings page could not
// show it, because each one timed a single strip: the "41ms" it reported was
// one seventh of a frame's projection work plus that strip's share of the
// drawing.
//
// So a build projects the view once into s_segments, keyed by everything the
// projection depends on, and each strip draws only the segments that cross it.
// The key earns a second saving: a redraw that changes nothing about the view
// -- the clock or a label in the status strip over the map, the corner status
// on the ROUTE page -- reuses the last build rather than redoing all of it to
// repaint a 30px patch.

// Water, then minor, then secondary, then arteries -- painter's order, so a
// trunk road crosses a river rather than being cut by it.
const uint8_t ORDER[ROAD_CLASS_COUNT] = {ROAD_CLASS_WATER, ROAD_CLASS_MINOR,
                                         ROAD_CLASS_SECONDARY, ROAD_CLASS_ARTERY};

struct RoadSegment {
    lv_point_t a;
    lv_point_t b;
};

// Sized to the larger of the two ceilings, since both bound what a build can
// keep: 900 segments at 8 bytes, in internal RAM because every strip walks it.
RoadSegment s_segments[ROAD_MAX_SEGMENTS];
// Class k's segments are [s_class_from[k], s_class_to[k]).
uint16_t s_class_from[ROAD_CLASS_COUNT];
uint16_t s_class_to[ROAD_CLASS_COUNT];

// Everything a build depends on. The layer, because two are attached at once;
// its absolute area, because the points are stored in screen coordinates and a
// page sliding in moves it; the map generation, because the route picker can
// load a different extract.
struct BuildKey {
    const lv_obj_t *layer;
    lv_area_t area;
    double clat;
    double clon;
    double mpp;
    double heading;
    bool interactive;
    uint32_t map_generation;
};
BuildKey s_key;
bool s_have_build = false;

// Strips of the frame being drawn, and what they cost. Published when the next
// frame starts (Display_FrameSeq), so the figure is a whole frame rather than
// one strip -- and not several frames, which is what totalling them per build
// would give for a map that sits still under an updating clock.
uint32_t s_acc_frame = 0;
uint32_t s_draw_accum_us = 0;
uint16_t s_strips_accum = 0;
bool s_acc_interactive = false;

// Field by field: the struct has padding, and memcmp would compare it.
bool SameKey(const BuildKey &x, const BuildKey &y) {
    return x.layer == y.layer && x.area.x1 == y.area.x1 && x.area.y1 == y.area.y1 &&
           x.area.x2 == y.area.x2 && x.area.y2 == y.area.y2 && x.clat == y.clat &&
           x.clon == y.clon && x.mpp == y.mpp && x.heading == y.heading &&
           x.interactive == y.interactive && x.map_generation == y.map_generation;
}

// Projects the view into s_segments. Everything here used to run inside the
// draw callback, once per strip.
void Build(const BuildKey &key) {
    const lv_area_t &area = key.area;
    const lv_coord_t w = lv_area_get_width(&area);
    const lv_coord_t h = lv_area_get_height(&area);
    const double clat = key.clat;
    const double clon = key.clon;
    const double mpp = key.mpp;

    // What the view covers, in degrees, so a way can be rejected without
    // projecting any of it.
    //
    // Built from the view's DIAGONAL, not its width and height. A track-up map
    // turns under the rider, so the rectangle on screen sweeps a circle as it
    // rotates, and a box sized to half-width by half-height loses the corners
    // the moment the view is not square to north -- which on screen reads as
    // roads blinking out of existence rather than as a margin being too small.
    // The same box is used north-up: it costs a few percent more ways and
    // means there is only one of these to be wrong.
    //
    // The extra tenth is for ways whose bounding box sits just outside while a
    // segment still crosses a corner.
    const double radius_px = Map_RotatedRadiusPx(w, h) * 1.1;
    const double half_h_deg = (radius_px * mpp) / MAP_EARTH_METRES_PER_DEGREE;
    const double cos_lat = cos(clat * M_PI / 180.0);
    const double half_w_deg =
        (radius_px * mpp) / (MAP_EARTH_METRES_PER_DEGREE * (cos_lat > 0.01 ? cos_lat : 0.01));
    const int32_t view_min_lat = (int32_t)((clat - half_h_deg) * ROADMAP_COORD_SCALE);
    const int32_t view_max_lat = (int32_t)((clat + half_h_deg) * ROADMAP_COORD_SCALE);
    const int32_t view_min_lon = (int32_t)((clon - half_w_deg) * ROADMAP_COORD_SCALE);
    const int32_t view_max_lon = (int32_t)((clon + half_w_deg) * ROADMAP_COORD_SCALE);

    // Prepared once for the whole build. The per-point path is then two
    // multiplies and two adds, where Map_Project would recompute a cosine and
    // two divisions for every point of every visible way.
    MapProjection_t proj;
    Map_PrepareProjection(&proj, clat, clon, mpp, (int16_t)(w / 2), (int16_t)(h / 2));
    // Taken from the view rather than recomputed, so the roads and the trail
    // turn by exactly the same angle. Two layers deriving the same number
    // independently is how they end up a frame apart.
    Map_SetProjectionHeading(&proj, key.heading);

    // Timed in two halves, because three rounds of optimising the drawing have
    // each moved the number less than expected. Guessing which half is
    // expensive has a poor record here; the split says outright.
    const uint32_t started = micros();
    uint32_t segments = 0;

    // Ask the index once, then project from what it returned.
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

    const uint32_t max_segments = key.interactive ? ROAD_DRAG_MAX_SEGMENTS : ROAD_MAX_SEGMENTS;
    const int min_segment_px = key.interactive ? ROAD_DRAG_MIN_SEGMENT_PX : ROAD_MIN_SEGMENT_PX;

    // The pixel threshold expressed back in scaled source degrees, so a point
    // can be rejected without being projected. px_per_deg_* already folds in
    // the scale and the latitude cosine; a zero or absurd value means the
    // projection is not usable, and decimating nothing is the safe answer.
    int32_t skip_rlat = 0;
    int32_t skip_rlon = 0;
    if (proj.px_per_deg_lat > 1e-9 && proj.px_per_deg_lon > 1e-9) {
        const double lat_deg = (double)min_segment_px / proj.px_per_deg_lat;
        const double lon_deg = (double)min_segment_px / proj.px_per_deg_lon;
        const double lat_raw = lat_deg * ROADMAP_COORD_SCALE;
        const double lon_raw = lon_deg * ROADMAP_COORD_SCALE;
        if (lat_raw > 0.0 && lat_raw < 2.0e9) {
            skip_rlat = (int32_t)lat_raw;
        }
        if (lon_raw > 0.0 && lon_raw < 2.0e9) {
            skip_rlon = (int32_t)lon_raw;
        }
    }

    for (int k = 0; k < ROAD_CLASS_COUNT; k++) {
        s_class_from[k] = 0;
        s_class_to[k] = 0;
    }

    for (int pass = 0; pass < ROAD_CLASS_COUNT && segments < max_segments; pass++) {
        const uint8_t klass = ORDER[pass];
        const uint16_t from = slice_start[klass];
        const uint16_t to = (uint16_t)(from + class_count[klass]);
        uint32_t class_segments = 0;
        s_class_from[klass] = (uint16_t)segments;

        for (uint16_t v = from; v < to && segments < max_segments &&
                                class_segments < ROAD_CLASS_SEGMENTS[klass];
             v++) {
            RoadWay_t way;
            if (!RoadMap_Way(sorted[v], &way)) {
                continue;
            }
            // Decimate while projecting: a point that lands within a pixel or
            // two of the last one kept cannot change what appears, and
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
            int32_t prev_rlat = 0;
            int32_t prev_rlon = 0;
            for (uint16_t k = 0; k < way.count && segments < max_segments &&
                                 class_segments < ROAD_CLASS_SEGMENTS[klass];
                 k++) {
                const int32_t rlat = way.points[k * 2];
                const int32_t rlon = way.points[k * 2 + 1];

                // ⚠️ Decimate BEFORE projecting, not after.
                //
                // This used to project every point and then throw most of
                // them away, which made the threshold a saving on
                // lv_draw_line and no saving at all on the work that actually
                // dominates. OSM is metre-resolution geometry: at any usable
                // zoom the large majority of points fall inside the threshold,
                // so the large majority of projections were computed in order
                // to be discarded.
                //
                // And a projection was not cheap here: the ESP32-S3 has only a
                // single-precision FPU, and the degree path converts and
                // subtracts in double -- see MapProject.h. ~214 visible ways at
                // tens of points each is around ten thousand of those.
                //
                // The measurement that found this: raising the threshold cut
                // segments 498 -> 264 and the time went UP, 35ms -> 47ms. A
                // cost that does not fall when you halve the thing you are
                // charging it to is not that thing's cost.
                //
                // The test is a box in source units rather than |dx|+|dy| in
                // pixels. Slightly more conservative, exact enough at these
                // scales, and it costs two integer compares against a
                // projection.
                if (have_prev && k + 1 < way.count) {
                    const int32_t dlat = rlat > prev_rlat ? rlat - prev_rlat : prev_rlat - rlat;
                    const int32_t dlon = rlon > prev_rlon ? rlon - prev_rlon : prev_rlon - rlon;
                    if (dlat < skip_rlat && dlon < skip_rlon) {
                        continue;
                    }
                }

                // Straight from the stored 1e-7 degrees: no conversion to a
                // double degree and no double subtraction, which were most of
                // what a point cost (MapProject.h, Map_ProjectE7Prepared).
                // ROADMAP_COORD_SCALE is 1e7, which is what makes that legal.
                int16_t x, y;
                Map_ProjectE7Prepared(&proj, rlat, rlon, &x, &y);
                lv_point_t p = {(lv_coord_t)(area.x1 + x), (lv_coord_t)(area.y1 + y)};

                const uint8_t code = OutCode(&p, &area);

                if (have_prev) {
                    // Both ends off the same side: the segment cannot cross
                    // the view, so there is nothing for LVGL to clip.
                    if ((code & prev_code) == 0) {
                        s_segments[segments].a = prev;
                        s_segments[segments].b = p;
                        segments++;
                        class_segments++;
                    }
                }
                prev = p;
                prev_code = code;
                prev_rlat = rlat;
                prev_rlon = rlon;
                have_prev = true;
            }
        }
        s_class_to[klass] = (uint16_t)segments;
    }

    g_visible = visible_count;
    g_segments = segments;
    g_build_us = micros() - started;
    if (key.interactive) {
        g_build_us_drag = g_build_us;
        g_segments_drag = (uint16_t)segments;
    }
}

// Draws the current build into one strip. A segment whose ink cannot reach the
// strip is skipped here with four compares, rather than handed to lv_draw_line
// to discover the same thing through its own setup.
void DrawBuild(lv_draw_ctx_t *ctx) {
    const lv_area_t *clip = ctx->clip_area;
    for (int pass = 0; pass < ROAD_CLASS_COUNT; pass++) {
        const uint8_t klass = ORDER[pass];
        const uint16_t from = s_class_from[klass];
        const uint16_t to = s_class_to[klass];
        if (from == to) {
            continue;
        }

        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.color = lv_color_hex(ROAD_STYLE[klass].colour);
        dsc.width = ROAD_STYLE[klass].width;
        dsc.round_start = 1;
        dsc.round_end = 1;

        // The full width rather than half of it, plus one: the round caps
        // reach past the endpoints, and anti-aliasing past the stroke. Too
        // generous costs a call that draws nothing; too tight leaves a seam
        // along the strip boundary.
        const lv_coord_t pad = (lv_coord_t)(ROAD_STYLE[klass].width + 1);
        for (uint16_t i = from; i < to; i++) {
            const RoadSegment &seg = s_segments[i];
            const lv_coord_t min_x = seg.a.x < seg.b.x ? seg.a.x : seg.b.x;
            const lv_coord_t max_x = seg.a.x < seg.b.x ? seg.b.x : seg.a.x;
            const lv_coord_t min_y = seg.a.y < seg.b.y ? seg.a.y : seg.b.y;
            const lv_coord_t max_y = seg.a.y < seg.b.y ? seg.b.y : seg.a.y;
            if (max_y + pad < clip->y1 || min_y - pad > clip->y2 || max_x + pad < clip->x1 ||
                min_x - pad > clip->x2) {
                continue;
            }
            lv_draw_line(ctx, &dsc, &seg.a, &seg.b);
        }
    }
}

void RoadDrawCb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    MapView_t *g_view = (MapView_t *)lv_obj_get_user_data(obj);
    if (!RoadMap_IsLoaded() || g_view == nullptr) {
        return;
    }
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);

    BuildKey key;
    key.layer = obj;
    lv_obj_get_coords(obj, &key.area);

    if (g_view->have_center) {
        key.clat = g_view->center_lat;
        key.clon = g_view->center_lon;
        key.mpp = g_view->metres_per_pixel;
    } else {
        // Nothing has set a view yet -- no fix, no track. Frame the roads
        // themselves so the map is not simply blank while waiting.
        double min_lat, min_lon, max_lat, max_lon;
        if (!RoadMap_Bounds(&min_lat, &min_lon, &max_lat, &max_lon)) {
            return;
        }
        key.clat = (min_lat + max_lat) / 2.0;
        key.clon = (min_lon + max_lon) / 2.0;
        key.mpp = Map_FitScale(min_lat, max_lat, min_lon, max_lon,
                               lv_area_get_width(&key.area), lv_area_get_height(&key.area), 4);
    }
    if (key.mpp <= 0.0) {
        return;
    }
    key.heading = MapView_HeadingDeg(g_view);
    key.interactive = g_interactive;
    key.map_generation = RoadMap_Generation();

    // A new frame: the strips counted so far were the last one, so publish
    // them as one figure and start again.
    const uint32_t frame = Display_FrameSeq();
    if (frame != s_acc_frame) {
        if (s_strips_accum > 0) {
            g_frame_draw_us = s_draw_accum_us;
            g_frame_strips = s_strips_accum;
            if (s_acc_interactive) {
                g_frame_draw_us_drag = s_draw_accum_us;
            }
        }
        s_acc_frame = frame;
        s_draw_accum_us = 0;
        s_strips_accum = 0;
    }

    if (!s_have_build || !SameKey(key, s_key)) {
        Build(key);
        s_key = key;
        s_have_build = true;
    }

    const uint32_t started = micros();
    DrawBuild(ctx);
    s_draw_accum_us += micros() - started;
    if (s_strips_accum < UINT16_MAX) {
        s_strips_accum++;
    }
    s_acc_interactive = key.interactive;
}

} // namespace

void RoadView_Attach(MapView_t *view) {
    if (view == nullptr || view->container == nullptr || !RoadMap_IsLoaded()) {
        return;
    }

    // Idempotent, which it has to be now that it is called again whenever an
    // extract is loaded late (Page_Map's route picker). Without this a second
    // call stacks another full-size transparent layer on the first: both stay
    // registered, both invalidate, and both draw every road twice.
    //
    // The pairing is by view, not by container, because the view is what the
    // draw callback projects through -- two layers over one container would
    // be the duplicate; one layer per view is the invariant.
    for (int i = 0; i < MAX_LAYERS; i++) {
        if (g_layers[i] != nullptr && lv_obj_get_user_data(g_layers[i]) == view) {
            return;
        }
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

void RoadView_SetInteractive(bool interactive) {
    if (g_interactive == interactive) {
        return;
    }
    g_interactive = interactive;
    // Repaint on the way OUT of a drag, so full detail comes back the instant
    // the finger lifts. On the way in, the pan that set this is about to
    // invalidate anyway.
    if (!interactive) {
        RoadView_Refresh();
    }
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

uint32_t RoadView_LastSegments() {
    return g_segments;
}

uint32_t RoadView_LastVisibleWays() {
    return g_visible;
}

uint32_t RoadView_LastCullUs() {
    return g_cull_us;
}

uint32_t RoadView_LastBuildUs() {
    return g_build_us;
}

uint32_t RoadView_LastFrameDrawUs() {
    return g_frame_draw_us;
}

uint16_t RoadView_LastFrameStrips() {
    return g_frame_strips;
}

uint32_t RoadView_LastDragBuildUs() {
    return g_build_us_drag;
}

uint32_t RoadView_LastDragFrameDrawUs() {
    return g_frame_draw_us_drag;
}

uint16_t RoadView_LastDragSegments() {
    return g_segments_drag;
}
