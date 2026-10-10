/* Host tests for the breadcrumb projection (src/navigation/MapProject.c).
 *
 * Geometry is easy to get subtly wrong in ways that still look like a map:
 * an inverted axis, a missing cos(latitude), a scale that fits one axis and
 * clips the other. Each is pinned separately below. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "MapProject.h"

static int checks = 0;
static int failures = 0;

static void check(int condition, const char *what) {
    checks++;
    if (!condition) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

static void check_near(int got, int want, int tol, const char *what) {
    checks++;
    if (abs(got - want) > tol) {
        failures++;
        printf("  FAIL: %s (got %d, want %d)\n", what, got, want);
    }
}

#define CAP 512
static int32_t lat_store[CAP];
static int32_t lon_store[CAP];
static MapPoint_t poly[CAP];

// ---------------------------------------------------------------------------
// The polyline builder, as it was before 2026-10-07
// ---------------------------------------------------------------------------
// Kept here verbatim in shape, as the reference the fast search is pinned
// against. The rewrite stopped projecting every point to find the visible
// range -- two scans inward, with an integer box in front -- and the whole
// claim is that it produces the SAME two indices and therefore the same
// polyline. A claim like that is either tested or it is a hope: the failure
// mode is an off-by-one in a scan bound, which draws a slightly wrong stretch
// of route on a screen nobody is comparing against a reference.
//
// Map_PointVisible is static in MapProject.c, so its rule is restated rather
// than called. That is deliberate too -- if someone widens the margin there
// and not here, these tests should be what notices.
static int RefVisible(int x, int y, int cx, int cy) {
    const int margin = 8;
    return x >= -margin && y >= -margin && x <= cx * 2 + margin && y <= cy * 2 + margin;
}

static size_t RefBuildPolyline(const TrackBuffer_t *track, const MapProjection_t *proj,
                               MapPoint_t *out, size_t max_points) {
    size_t written = 0;
    size_t first = 0;
    size_t last = 0;
    int found = 0;
    int16_t last_x = 0;
    int16_t last_y = 0;
    size_t span;
    size_t stride;

    if (track == NULL || proj == NULL || out == NULL || max_points == 0 || track->count == 0) {
        return 0;
    }

    for (size_t i = 0; i < track->count; i++) {
        double lat, lon;
        int16_t x, y;
        if (!TrackBuffer_Get(track, i, &lat, &lon)) {
            continue;
        }
        Map_ProjectPrepared(proj, lat, lon, &x, &y);
        if (!RefVisible(x, y, proj->center_x, proj->center_y)) {
            continue;
        }
        if (!found) {
            first = i;
            found = 1;
        }
        last = i;
    }
    if (!found) {
        return 0;
    }

    if (first > 0) {
        first--;
    }
    if (last + 1 < track->count) {
        last++;
    }

    span = last - first + 1;
    stride = (span + max_points - 1) / max_points;
    if (stride == 0) {
        stride = 1;
    }

    for (size_t i = first; i <= last; i += stride) {
        double lat, lon;
        int16_t x, y;
        if (!TrackBuffer_Get(track, i, &lat, &lon)) {
            continue;
        }
        Map_ProjectPrepared(proj, lat, lon, &x, &y);
        if (written > 0 && x == last_x && y == last_y) {
            continue;
        }
        out[written].x = x;
        out[written].y = y;
        last_x = x;
        last_y = y;
        written++;
        if (written >= max_points) {
            break;
        }
    }
    return written;
}

// Big enough that TrackBuffer_Add never halves and thins the input: the
// thinning is TrackBuffer's own business and tested there, and letting it fire
// here would compare two builders against a track neither of them chose.
#define REF_CAP 6000
static int32_t s_ref_lat[REF_CAP];
static int32_t s_ref_lon[REF_CAP];

typedef enum {
    SHAPE_DIAGONAL,     /* one long straight run */
    SHAPE_OUT_AND_BACK, /* the same points forward then reversed */
    SHAPE_LOOP,
    SHAPE_EXCURSION,    /* leaves the area entirely and comes back */
    SHAPE_SINGLE,
    SHAPE_COUNT
} RefShape_t;

static void BuildShape(TrackBuffer_t *track, RefShape_t shape, double lat0, double lon0) {
    TrackBuffer_Init(track, s_ref_lat, s_ref_lon, REF_CAP);

    switch (shape) {
    case SHAPE_DIAGONAL:
        for (int i = 0; i < 5000; i++) {
            TrackBuffer_Add(track, lat0 + i * 0.00002, lon0 + i * 0.00003);
        }
        break;
    case SHAPE_OUT_AND_BACK:
        for (int i = 0; i < 1200; i++) {
            TrackBuffer_Add(track, lat0 + i * 0.00004, lon0 + i * 0.00001);
        }
        for (int i = 1200; i-- > 0;) {
            TrackBuffer_Add(track, lat0 + i * 0.00004, lon0 + i * 0.00001);
        }
        break;
    case SHAPE_LOOP:
        for (int i = 0; i < 2000; i++) {
            const double t = (double)i / 2000.0 * 6.28318530718;
            TrackBuffer_Add(track, lat0 + 0.01 * sin(t), lon0 + 0.014 * cos(t));
        }
        break;
    case SHAPE_EXCURSION:
        for (int i = 0; i < 400; i++) {
            TrackBuffer_Add(track, lat0 + i * 0.00003, lon0 + i * 0.00003);
        }
        for (int i = 0; i < 400; i++) {
            /* Kilometres away: every one of these is culled, and the drawn
               line must not chord across the gap. */
            TrackBuffer_Add(track, lat0 + 0.4 + i * 0.0001, lon0 + 0.5 + i * 0.0001);
        }
        for (int i = 0; i < 400; i++) {
            TrackBuffer_Add(track, lat0 + 0.012 + i * 0.00003, lon0 + 0.012 + i * 0.00003);
        }
        break;
    case SHAPE_SINGLE:
        TrackBuffer_Add(track, lat0, lon0);
        break;
    default:
        break;
    }
}

static void test_fast_search_matches_the_old_full_pass(void) {
    printf("- the fast visible-range search draws what the full pass drew: ");

    static MapPoint_t got[512];
    static MapPoint_t want[512];

    /* Centres: on the track, beside it, and far enough away that nothing is
       visible at all. Scales from "a driveway" to "the whole county". */
    const double anchors[][2] = {{38.8800, -77.1400}, {0.0, 0.0}, {60.2000, 24.9000},
                                 {-33.9000, 151.2000}};
    const double scales[] = {0.5, 3.0, 17.5, 80.0, 400.0};
    const double headings[] = {0.0, 37.0, 90.0, 180.0, 271.0};
    const double offsets[][2] = {{0.0, 0.0}, {0.004, 0.004}, {-0.02, 0.03}, {5.0, 5.0}};
    const size_t budgets[] = {64, 256, 512};
    const int16_t halves[][2] = {{120, 131}, {120, 100}, {40, 40}};

    int compared = 0;

    for (size_t a = 0; a < sizeof(anchors) / sizeof(anchors[0]); a++) {
        for (int shape = 0; shape < SHAPE_COUNT; shape++) {
            TrackBuffer_t track;
            BuildShape(&track, (RefShape_t)shape, anchors[a][0], anchors[a][1]);

            for (size_t s = 0; s < sizeof(scales) / sizeof(scales[0]); s++) {
                for (size_t h = 0; h < sizeof(headings) / sizeof(headings[0]); h++) {
                    for (size_t o = 0; o < sizeof(offsets) / sizeof(offsets[0]); o++) {
                        for (size_t b = 0; b < sizeof(budgets) / sizeof(budgets[0]); b++) {
                            for (size_t v = 0; v < sizeof(halves) / sizeof(halves[0]); v++) {
                                MapProjection_t proj;
                                Map_PrepareProjection(&proj, anchors[a][0] + offsets[o][0],
                                                      anchors[a][1] + offsets[o][1], scales[s],
                                                      halves[v][0], halves[v][1]);
                                Map_SetProjectionHeading(&proj, headings[h]);

                                const size_t n_want =
                                    RefBuildPolyline(&track, &proj, want, budgets[b]);
                                const size_t n_got =
                                    Map_BuildPolylinePrepared(&track, &proj, got, budgets[b]);

                                if (n_got != n_want) {
                                    check(0, "same number of drawn points as the full pass");
                                    continue;
                                }
                                int same = 1;
                                for (size_t i = 0; i < n_want; i++) {
                                    if (got[i].x != want[i].x || got[i].y != want[i].y) {
                                        same = 0;
                                        break;
                                    }
                                }
                                check(same, "same drawn points as the full pass");
                                compared++;
                            }
                        }
                    }
                }
            }
        }
    }

    /* A test that compared nothing would pass silently. */
    check(compared > 3000, "the sweep actually ran");

    /* And at least one configuration must have drawn something, or the whole
       sweep could be agreeing on "nothing visible" everywhere. */
    {
        TrackBuffer_t track;
        BuildShape(&track, SHAPE_DIAGONAL, 38.88, -77.14);
        MapProjection_t proj;
        Map_PrepareProjection(&proj, 38.88 + 0.001, -77.14 + 0.001, 3.0, 120, 131);
        check(Map_BuildPolylinePrepared(&track, &proj, got, 256) > 10,
              "a centred track draws a real line");
    }
    printf("done\n");
}

static void test_source_box_admits_the_rotated_corners(void) {
    printf("- a turned view keeps the corners it would otherwise lose: ");

    static MapPoint_t got[512];
    static MapPoint_t want[512];

    /* A point in the corner of a rotated viewport is outside the box an
       unrotated half-extent pair would build, and inside the view. If the
       prefilter were built from half-extents rather than the half-diagonal,
       this is where the drawn line would start late or end early. */
    TrackBuffer_t track;
    TrackBuffer_Init(&track, s_ref_lat, s_ref_lon, REF_CAP);
    const double lat0 = 38.88;
    const double lon0 = -77.14;
    for (int i = 0; i < 900; i++) {
        /* A long diagonal, which is the direction a square viewport's
           diagonal reaches furthest. */
        TrackBuffer_Add(&track, lat0 + (i - 450) * 0.00002, lon0 + (i - 450) * 0.000025);
    }

    for (int deg = 0; deg < 360; deg += 15) {
        MapProjection_t proj;
        Map_PrepareProjection(&proj, lat0, lon0, 2.0, 120, 131);
        Map_SetProjectionHeading(&proj, (double)deg);

        const size_t n_want = RefBuildPolyline(&track, &proj, want, 256);
        const size_t n_got = Map_BuildPolylinePrepared(&track, &proj, got, 256);
        check(n_got == n_want, "rotated: same count");
        if (n_got == n_want) {
            int same = 1;
            for (size_t i = 0; i < n_want; i++) {
                if (got[i].x != want[i].x || got[i].y != want[i].y) {
                    same = 0;
                    break;
                }
            }
            check(same, "rotated: same points");
        }
    }
    printf("done\n");
}

static void test_degenerate_scale_still_builds(void) {
    printf("- a zero scale does not take the box path: ");
    /* px_per_deg is zero, so no box can be built and every point has to stay
       a candidate. Without that fallback the prefilter would reject the whole
       track and the map would go blank rather than collapse to a dot. */
    static MapPoint_t got[64];
    TrackBuffer_t track;
    BuildShape(&track, SHAPE_DIAGONAL, 38.88, -77.14);

    MapProjection_t proj;
    Map_PrepareProjection(&proj, 38.88, -77.14, 0.0, 120, 131);
    const size_t n = Map_BuildPolylinePrepared(&track, &proj, got, 64);
    check(n == 1, "every point pins to the centre and collapses to one");
    if (n >= 1) {
        check(got[0].x == 120 && got[0].y == 131, "and that point is the centre");
    }
    printf("done\n");
}

// ---------------------------------------------------------------------------
// The integer projection against the degree one
// ---------------------------------------------------------------------------
// Map_ProjectE7Prepared exists to take every double out of the road layer's
// per-point path. The claim is that it agrees with Map_ProjectPrepared to the
// pixel except where the two round a value lying within float precision of a
// half. "Except" has to be measured, not asserted, so this counts.
//
// A small deterministic generator rather than rand(), so a failure reproduces.
static uint32_t s_lcg = 12345u;
static int32_t NextSpread(int32_t spread) {
    s_lcg = s_lcg * 1664525u + 1013904223u;
    return (int32_t)(s_lcg % (uint32_t)(2 * spread + 1)) - spread;
}

static void test_e7_projection_agrees(void) {
    printf("- e7 projection agrees with the degree projection: ");
    const double anchors[][2] = {
        {0.0, 0.0},          // equator, where longitude is widest
        {39.17, -77.27},     // the riding area
        {-33.87, 151.21},    // the other hemisphere and sign
        {64.15, -21.94},     // far north, where cos(lat) bites
        {10.0, 179.95},      // beside the antimeridian
    };
    const double scales[] = {0.3, 1.0, 4.0, 16.0, 80.0};
    const double headings[] = {0.0, 37.0, 90.0, 180.0, 271.5};
    long compared = 0;
    long differed = 0;
    int worst = 0;

    for (size_t a = 0; a < sizeof(anchors) / sizeof(anchors[0]); a++) {
        for (size_t sc = 0; sc < sizeof(scales) / sizeof(scales[0]); sc++) {
            for (size_t hd = 0; hd < sizeof(headings) / sizeof(headings[0]); hd++) {
                MapProjection_t proj;
                // A centre that is not on a whole 1e-7 unit, as a fix never is.
                const double clat = anchors[a][0] + 0.000000037;
                const double clon = anchors[a][1] - 0.000000061;
                Map_PrepareProjection(&proj, clat, clon, scales[sc], 120, 131);
                Map_SetProjectionHeading(&proj, headings[hd]);

                // Points spread over a few screens at this scale, so most land
                // on or near the view and some are clamped.
                const int32_t spread = (int32_t)(scales[sc] * 600.0 / 111320.0 * 1e7);
                for (int i = 0; i < 400; i++) {
                    const int32_t lat_e7 = (int32_t)lround(clat * 1e7) + NextSpread(spread);
                    const int32_t lon_e7 = (int32_t)lround(clon * 1e7) + NextSpread(spread);
                    int16_t ex, ey, dx, dy;
                    Map_ProjectE7Prepared(&proj, lat_e7, lon_e7, &ex, &ey);
                    Map_ProjectPrepared(&proj, (double)lat_e7 * 1e-7, (double)lon_e7 * 1e-7,
                                        &dx, &dy);
                    const int ddx = ex > dx ? ex - dx : dx - ex;
                    const int ddy = ey > dy ? ey - dy : dy - ey;
                    const int d = ddx > ddy ? ddx : ddy;
                    compared++;
                    if (d != 0) {
                        differed++;
                    }
                    if (d > worst) {
                        worst = d;
                    }
                }
            }
        }
    }
    check(worst <= 1, "never more than a pixel apart");
    // A rounding coincidence, not a habit: well under one point in a thousand.
    check(differed * 1000 < compared, "and almost never apart at all");
    printf("done (%ld points, %ld off by a pixel)\n", compared, differed);

    {
        // The degenerate scale pins to the centre on this path too.
        MapProjection_t bad;
        int16_t bx, by;
        Map_PrepareProjection(&bad, 38.0, -77.0, 0.0, 55, 66);
        Map_ProjectE7Prepared(&bad, 390000000, -780000000, &bx, &by);
        check(bx == 55 && by == 66, "zero scale pins to the centre (e7)");
    }
}

int main(void) {
    TrackBuffer_t track;
    int16_t x;
    int16_t y;

    printf("- the view centre projects to the centre pixel: ");
    Map_Project(38.8821, -77.0194, 38.8821, -77.0194, 1.0, 120, 160, &x, &y);
    check(x == 120 && y == 160, "centre maps to centre");
    printf("done\n");

    printf("- north is up and east is right: ");
    /* 100m north at 1 m/px must be 100px UP the screen, i.e. a SMALLER y --
       latitude grows north while screen y grows downward. */
    Map_Project(38.8821 + 100.0 / MAP_EARTH_METRES_PER_DEGREE, -77.0194, 38.8821, -77.0194, 1.0,
                120, 160, &x, &y);
    check_near(y, 60, 1, "100m north is 100px up");
    check_near(x, 120, 1, "and does not move sideways");

    Map_Project(38.8821, -77.0194 + 100.0 / (MAP_EARTH_METRES_PER_DEGREE * cos(38.8821 * M_PI / 180.0)),
                38.8821, -77.0194, 1.0, 120, 160, &x, &y);
    check_near(x, 220, 1, "100m east is 100px right");
    check_near(y, 160, 1, "and does not move vertically");
    printf("done\n");

    printf("- longitude is scaled by cos(latitude): ");
    {
        int16_t x_equator;
        int16_t x_north;
        int16_t dummy;
        /* The same longitude delta covers far less ground at 60N than at the
           equator. Without the cosine the trail would be stretched sideways
           by a factor of two up there. */
        Map_Project(0.0, 0.01, 0.0, 0.0, 1.0, 0, 0, &x_equator, &dummy);
        Map_Project(60.0, 0.01, 60.0, 0.0, 1.0, 0, 0, &x_north, &dummy);
        check(x_north < x_equator, "same delta is narrower at 60N");
        /* cos(60) = 0.5 exactly, so it should be about half. */
        check_near(x_north, x_equator / 2, 2, "and about half as wide");
    }
    printf("done\n");

    printf("- scale is chosen by the tighter axis: ");
    {
        /* A track wide in longitude but shallow in latitude must be fitted by
           its width, or it runs off the sides. */
        double wide = Map_FitScale(38.88, 38.881, -77.05, -77.00, 240, 320, 10);
        double tall = Map_FitScale(38.80, 38.90, -77.02, -77.019, 240, 320, 10);
        double span_lon_m = 0.05 * MAP_EARTH_METRES_PER_DEGREE * cos(38.88 * M_PI / 180.0);
        double span_lat_m = 0.10 * MAP_EARTH_METRES_PER_DEGREE;
        check(fabs(wide - span_lon_m / 220.0) < 1.0, "wide track fitted by width");
        check(fabs(tall - span_lat_m / 300.0) < 1.0, "tall track fitted by height");
    }
    printf("done\n");

    printf("- a fitted track lands inside the viewport: ");
    {
        const double min_lat = 38.870, max_lat = 38.895;
        const double min_lon = -77.040, max_lon = -77.000;
        const double mpp = Map_FitScale(min_lat, max_lat, min_lon, max_lon, 240, 320, 10);
        const double clat = (min_lat + max_lat) / 2.0;
        const double clon = (min_lon + max_lon) / 2.0;
        int inside = 1;
        double lat;
        double lon;
        /* Walk the four corners of the bounding box. */
        for (lat = min_lat; lat <= max_lat + 1e-9; lat += (max_lat - min_lat)) {
            for (lon = min_lon; lon <= max_lon + 1e-9; lon += (max_lon - min_lon)) {
                Map_Project(lat, lon, clat, clon, mpp, 120, 160, &x, &y);
                if (x < 0 || x > 240 || y < 0 || y > 320) {
                    inside = 0;
                }
            }
        }
        check(inside, "every corner of the bounds is on screen");
    }
    printf("done\n");

    printf("- a zero-extent track still gets a usable scale: ");
    {
        /* A rider standing still, or a one-point file: the span is zero and a
           naive fit would divide by it. */
        double mpp = Map_FitScale(38.88, 38.88, -77.02, -77.02, 240, 320, 10);
        check(mpp > 0.0, "scale is positive");
        Map_Project(38.88, -77.02, 38.88, -77.02, mpp, 120, 160, &x, &y);
        check(x == 120 && y == 160, "and projection still works");
    }
    printf("done\n");

    printf("- far-away points clamp instead of wrapping: ");
    /* A point on another continent at 1 m/px would be tens of millions of
       pixels away; int16 would wrap it back across the screen as a line to
       nowhere. */
    Map_Project(0.0, 0.0, 38.8821, -77.0194, 1.0, 120, 160, &x, &y);
    check(x == -MAP_COORD_LIMIT || x == MAP_COORD_LIMIT, "x clamped to the limit");
    check(y == MAP_COORD_LIMIT, "y clamped, still pointing south");
    printf("done\n");

    printf("- the polyline collapses points sharing a pixel: ");
    TrackBuffer_Init(&track, lat_store, lon_store, CAP);
    {
        int i;
        /* 200 points at a standstill, then one 50m away. */
        for (i = 0; i < 200; i++) {
            TrackBuffer_Add(&track, 38.8821, -77.0194);
        }
        TrackBuffer_Add(&track, 38.8821 + 50.0 / MAP_EARTH_METRES_PER_DEGREE, -77.0194);
    }
    {
        size_t n = Map_BuildPolyline(&track, 38.8821, -77.0194, 1.0, 120, 160, poly, CAP);
        check(n == 2, "200 stationary points collapse to one");
        check(poly[0].x == 120 && poly[0].y == 160, "first is the centre");
        check_near(poly[1].y, 110, 1, "second is 50px north");
    }
    printf("done\n");

    printf("- the polyline respects its output limit: ");
    TrackBuffer_Init(&track, lat_store, lon_store, CAP);
    {
        int i;
        for (i = 0; i < 400; i++) {
            TrackBuffer_Add(&track, 38.80 + (double)i * 0.001, -77.0);
        }
        /* Never more than the buffer, and never zero when part of the track is
           on screen. Not exactly max_points: the visible range is thinned to
           fit rather than truncated, and thinning can land two kept points on
           one pixel, which the collapsing below then merges. */
        {
            const size_t n = Map_BuildPolyline(&track, 38.85, -77.0, 55.0, 120, 160, poly, 16);
            check(n > 0 && n <= 16, "fills the buffer without exceeding it");
        }
        check(Map_BuildPolyline(&track, 38.85, -77.0, 55.0, 120, 160, poly, 0) == 0,
              "zero capacity writes nothing");
        check(Map_BuildPolyline(&track, 38.85, -77.0, 1.0, 120, 160, poly, 16) <= 16,
              "never exceeds max_points, however much is off screen");
    }
    printf("done\n");

    printf("- a small buffer still reaches the visible part of a long track: ");
    /* The bug this guards: the walk used to fill the buffer with the START of
       the track and stop. Zoom in far enough that the visible stretch needs
       more points than the buffer holds, and the map drew a piece of the
       track's beginning and nothing where the rider actually was -- an empty
       map, which looks like the trail failing to load rather than a projection
       running out of room. */
    TrackBuffer_Init(&track, lat_store, lon_store, CAP);
    {
        int i;
        int inside = 0;
        size_t written;
        size_t j;
        for (i = 0; i < 400; i++) {
            TrackBuffer_Add(&track, 38.80 + (double)i * 0.001, -77.0);
        }
        /* Centred near the END of the track, zoomed in hard, with room for far
           fewer points than the track holds. */
        written = Map_BuildPolyline(&track, 39.19, -77.0, 1.0, 120, 160, poly, 16);
        for (j = 0; j < written; j++) {
            if (poly[j].x >= 0 && poly[j].x <= 240 && poly[j].y >= 0 && poly[j].y <= 320) {
                inside++;
            }
        }
        check(written > 0, "something is drawn");
        check(inside > 0, "and some of it is on screen");
    }
    printf("done\n");

    printf("- a track that leaves the screen and returns draws no shortcut: ");
    /* The bug this guards, which was the fix for the previous one gone wrong:
       collapsing an off-screen excursion into a single point drew a straight
       chord from where the track left the viewport to where it came back,
       cutting clean across a map the route never crosses. On the panel it was
       a green diagonal through the middle of the town.

       The property that rules it out is local: consecutive drawn points come
       from consecutive track points, so with a dense track none of them should
       be far apart. A chord shows up as one enormous step. */
    TrackBuffer_Init(&track, lat_store, lon_store, CAP);
    {
        int i;
        size_t written;
        size_t j;
        int32_t worst = 0;

        /* East until it leaves by the RIGHT edge, a long way round to the
           north, and back in through the LEFT edge. The two edges are what
           make this the case that matters: merging the excursion leaves one
           step from the right edge to the left, straight across everything in
           between. An out-and-back that leaves and returns by the same edge
           produces a short step and proves nothing. */
        for (i = 0; i < 60; i++) { /* out through the right edge */
            TrackBuffer_Add(&track, 38.9050, -77.0020 + (double)i * 0.00020);
        }
        for (i = 0; i < 80; i++) { /* north and west, far outside */
            TrackBuffer_Add(&track, 38.9050 + (double)i * 0.00025,
                            -76.9900 - (double)i * 0.00025);
        }
        for (i = 0; i < 60; i++) { /* back down and in through the left edge */
            TrackBuffer_Add(&track, 38.9250 - (double)i * 0.00033, -77.0100 + (double)i * 0.00007);
        }

        written = Map_BuildPolyline(&track, 38.9050, -77.0020, 3.0, 120, 160, poly, CAP);
        check(written > 4, "the excursion is drawn, not merged away");
        for (j = 1; j < written; j++) {
            const int32_t dx = (int32_t)poly[j].x - poly[j - 1].x;
            const int32_t dy = (int32_t)poly[j].y - poly[j - 1].y;
            const int32_t step = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
            if (step > worst) {
                worst = step;
            }
        }
        check(worst < 200, "no single step jumps across the viewport");
    }
    printf("done\n");

    printf("- null arguments: ");
    check(Map_BuildPolyline(NULL, 0, 0, 1.0, 0, 0, poly, CAP) == 0, "null track");
    check(Map_BuildPolyline(&track, 0, 0, 1.0, 0, 0, NULL, CAP) == 0, "null output");
    Map_Project(0, 0, 0, 0, 1.0, 0, 0, NULL, &y); /* must not crash */
    printf("done\n");

    printf("- track-up puts the heading at the top of the screen: ");
    {
        // 100m north, east, south and west of a centre, at 1 metre per pixel,
        // so every offset is 100px and the arithmetic is checkable by eye.
        const double clat = 39.1834, clon = -77.2617;
        const double mpp = 1.0;
        const double d_lat = 100.0 / MAP_EARTH_METRES_PER_DEGREE;
        const double d_lon = d_lat / cos(clat * M_PI / 180.0);
        const int16_t cx = 120, cy = 160;

        struct { const char *name; double lat; double lon; } around[] = {
            {"north", clat + d_lat, clon},
            {"east",  clat,         clon + d_lon},
            {"south", clat - d_lat, clon},
            {"west",  clat,         clon - d_lon},
        };

        MapProjection_t proj;
        int16_t x, y;

        // Heading 0 is north-up, which must be exactly what it was before.
        Map_PrepareProjection(&proj, clat, clon, mpp, cx, cy);
        Map_SetProjectionHeading(&proj, 0.0);
        Map_ProjectPrepared(&proj, around[0].lat, around[0].lon, &x, &y);
        check(x == cx && y < cy, "north-up: north is above the centre");

        // Heading 90 is riding east, so east must be at the top.
        Map_SetProjectionHeading(&proj, 90.0);
        Map_ProjectPrepared(&proj, around[1].lat, around[1].lon, &x, &y);
        check(abs(x - cx) <= 1 && y < cy - 90, "heading east: east is at the top");
        Map_ProjectPrepared(&proj, around[0].lat, around[0].lon, &x, &y);
        check(abs(y - cy) <= 1 && x < cx - 90, "...and north has swung to the left");

        // Heading 180 is riding south, so north is behind the rider.
        Map_SetProjectionHeading(&proj, 180.0);
        Map_ProjectPrepared(&proj, around[0].lat, around[0].lon, &x, &y);
        check(abs(x - cx) <= 1 && y > cy + 90, "heading south: north is at the bottom");

        // The centre is the one point rotation cannot move.
        Map_SetProjectionHeading(&proj, 217.0);
        Map_ProjectPrepared(&proj, clat, clon, &x, &y);
        check(x == cx && y == cy, "the rider stays in the middle at any angle");

        // Rotation is rigid: every point keeps its distance from the centre,
        // whatever the angle. A transform that scaled as it turned would show
        // up as the map breathing through a corner.
        for (int deg = 0; deg < 360; deg += 17) {
            Map_SetProjectionHeading(&proj, (double)deg);
            for (size_t k = 0; k < 4; k++) {
                Map_ProjectPrepared(&proj, around[k].lat, around[k].lon, &x, &y);
                const double r = sqrt((double)(x - cx) * (x - cx) + (double)(y - cy) * (y - cy));
                if (fabs(r - 100.0) > 2.0) {
                    check(0, "distance from the centre is preserved at every angle");
                    deg = 360;
                    break;
                }
            }
        }
        check(1, "distance from the centre is preserved at every angle");
    }
    printf("done\n");

    printf("- a rotated view sweeps its own diagonal: ");
    {
        // Half-diagonal of 240x320 is 200. A query box built from half-width
        // and half-height would be 120 by 160 and would lose the corners the
        // moment the view turned, which reads as roads vanishing.
        check(fabs(Map_RotatedRadiusPx(240, 320) - 200.0) < 0.01, "240x320 sweeps 200px");
        check(fabs(Map_RotatedRadiusPx(240, 180) - 150.0) < 0.01, "240x180 sweeps 150px");
        check(Map_RotatedRadiusPx(240, 320) > 320 / 2, "which is more than half the height");
    }
    printf("done\n");

    printf("- the prepared projection matches the reference exactly: ");
    {
        // The fast path exists because Map_Project recomputes a cosine and two
        // divisions per point, which is ruinous for a road map. It is only
        // safe if it agrees with the reference everywhere, so check it does --
        // across latitudes, scales and offsets, not at one convenient point.
        const double centres[][2] = {{38.88, -77.14}, {0.0, 0.0}, {60.2, 24.9}, {-33.9, 151.2}};
        const double scales[] = {1.0, 2.0, 17.5, 80.0};

        for (size_t c = 0; c < sizeof(centres) / sizeof(centres[0]); c++) {
            for (size_t s = 0; s < sizeof(scales) / sizeof(scales[0]); s++) {
                MapProjection_t proj;
                Map_PrepareProjection(&proj, centres[c][0], centres[c][1], scales[s], 120, 131);

                for (int dy = -3; dy <= 3; dy++) {
                    for (int dx = -3; dx <= 3; dx++) {
                        const double lat = centres[c][0] + dy * 0.004;
                        const double lon = centres[c][1] + dx * 0.004;

                        int16_t rx, ry, fx, fy;
                        Map_Project(lat, lon, centres[c][0], centres[c][1], scales[s], 120, 131,
                                    &rx, &ry);
                        Map_ProjectPrepared(&proj, lat, lon, &fx, &fy);
                        check(rx == fx && ry == fy, "prepared equals reference");
                    }
                }
            }
        }

        // A degenerate scale must land on the centre either way rather than
        // dividing by zero.
        MapProjection_t bad;
        Map_PrepareProjection(&bad, 38.0, -77.0, 0.0, 55, 66);
        int16_t bx, by;
        Map_ProjectPrepared(&bad, 39.0, -78.0, &bx, &by);
        check(bx == 55 && by == 66, "zero scale pins to the centre");
    }
    printf("done\n");

    test_fast_search_matches_the_old_full_pass();
    test_source_box_admits_the_rotated_corners();
    test_degenerate_scale_still_builds();
    test_e7_projection_agrees();

    printf("\nchecks: %d  failures: %d\n", checks, failures);
    printf("RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
