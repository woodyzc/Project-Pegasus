/* Host tests for the map's line rasterizer (src/ui/LineRaster.c).
 *
 * It writes straight into LVGL's draw buffer, which is the whole point of it
 * and the whole risk: a wrong interval draws a road a pixel fat or thin on
 * some rows, and a wrong clip writes outside the strip, into whatever memory
 * follows it. Both are checked for every pixel of every case, against a
 * reference that has none of the cleverness -- the distance from each pixel
 * centre to the segment, in double, with a square root per pixel. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "LineRaster.h"

static int checks = 0;
static int failures = 0;

static void check(int condition, const char *what) {
    checks++;
    if (!condition) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

static uint32_t rng_state = 12345u;
static uint32_t Rand(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static int32_t RandIn(int32_t lo, int32_t hi) {
    return lo + (int32_t)(Rand() % (uint32_t)(hi - lo + 1));
}

static uint16_t Swap16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

// ---------------------------------------------------------------------------
// The reference
// ---------------------------------------------------------------------------
// What LineRaster.h promises, written the slow and obvious way.
static double RefCoverage(int32_t x, int32_t y, int32_t ax, int32_t ay, int32_t bx, int32_t by,
                          int32_t width) {
    const double off = (width & 1) ? 0.0 : 0.5;
    const double r = width * 0.5;
    const double a_x = ax + off;
    const double a_y = ay + off;
    const double dx = (double)(bx - ax);
    const double dy = (double)(by - ay);
    const double len2 = dx * dx + dy * dy;
    double t = ((x - a_x) * dx + (y - a_y) * dy) / len2;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    const double cx = a_x + t * dx;
    const double cy = a_y + t * dy;
    const double d = sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
    double c = r + 0.5 - d;
    if (c < 0.0) c = 0.0;
    if (c > 1.0) c = 1.0;
    return c;
}

// ---------------------------------------------------------------------------
// A buffer to draw into, with a sentinel everywhere the clip does not reach
// ---------------------------------------------------------------------------
#define BUF_W 160
#define BUF_H 140

typedef struct {
    LineRasterSurface_t s;
    uint16_t *pixels; // exactly BUF_W * BUF_H, so ASan sees any overrun
} Canvas;

static const uint16_t SENTINEL = 0x5A5A;
static const uint16_t BLACK = 0x0000;
static const uint16_t WHITE = 0xFFFF;

static void CanvasInit(Canvas *c, int32_t origin_x, int32_t origin_y, int32_t cx1, int32_t cy1,
                       int32_t cx2, int32_t cy2, int swapped) {
    c->pixels = (uint16_t *)malloc(sizeof(uint16_t) * BUF_W * BUF_H);
    c->s.pixels = c->pixels;
    c->s.stride = BUF_W;
    c->s.origin_x = origin_x;
    c->s.origin_y = origin_y;
    c->s.clip_x1 = cx1;
    c->s.clip_y1 = cy1;
    c->s.clip_x2 = cx2;
    c->s.clip_y2 = cy2;
    c->s.swapped = swapped != 0;
    for (int32_t y = 0; y < BUF_H; y++) {
        for (int32_t x = 0; x < BUF_W; x++) {
            const int32_t sx = x + origin_x;
            const int32_t sy = y + origin_y;
            const int inside = sx >= cx1 && sx <= cx2 && sy >= cy1 && sy <= cy2;
            c->pixels[y * BUF_W + x] = inside ? BLACK : SENTINEL;
        }
    }
}

static void CanvasFree(Canvas *c) {
    free(c->pixels);
    c->pixels = NULL;
}

// Whether `got` is white drawn over black at an alpha within `tol` of `want`.
// Blending quantises alpha to 32 steps, so the alpha itself cannot be read
// back; the pixel each nearby alpha would give can.
static int PixelMatches(uint16_t got, double want_coverage, int tol) {
    const int want = (int)(want_coverage * 255.0 + 0.5);
    for (int a = want - tol; a <= want + tol; a++) {
        if (a < 0 || a > 255) continue;
        if (LineRaster_Blend(BLACK, WHITE, (uint8_t)a, 0) == got) return 1;
    }
    return 0;
}

// Draws one line and checks the whole canvas. Returns the number of bad
// pixels, printing the first.
static int DrawAndCompare(Canvas *c, int32_t ax, int32_t ay, int32_t bx, int32_t by,
                          int32_t width, const char *label) {
    const uint16_t colour = c->s.swapped ? Swap16(WHITE) : WHITE;
    LineRaster_Line(&c->s, ax, ay, bx, by, width, colour);
    int bad = 0;
    for (int32_t y = 0; y < BUF_H; y++) {
        for (int32_t x = 0; x < BUF_W; x++) {
            const int32_t sx = x + c->s.origin_x;
            const int32_t sy = y + c->s.origin_y;
            uint16_t got = c->pixels[y * BUF_W + x];
            const int inside = sx >= c->s.clip_x1 && sx <= c->s.clip_x2 &&
                               sy >= c->s.clip_y1 && sy <= c->s.clip_y2;
            if (!inside) {
                if (got != SENTINEL) {
                    if (bad++ == 0) {
                        printf("  %s: wrote OUTSIDE the clip at (%d,%d), line (%d,%d)-(%d,%d) w%d\n",
                               label, sx, sy, ax, ay, bx, by, width);
                    }
                }
                continue;
            }
            if (c->s.swapped) got = Swap16(got);
            const double want =
                (ax == bx && ay == by) ? 0.0 : RefCoverage(sx, sy, ax, ay, bx, by, width);
            if (!PixelMatches(got, want, 12)) {
                if (bad++ == 0) {
                    printf("  %s: pixel (%d,%d) = 0x%04X, want coverage %.3f; line (%d,%d)-(%d,%d) "
                           "w%d\n",
                           label, sx, sy, got, want, ax, ay, bx, by, width);
                }
            }
        }
    }
    return bad;
}

// ---------------------------------------------------------------------------
// Blending
// ---------------------------------------------------------------------------
// The fringe pixels are blended the way lv_color_mix does it, and the trick
// it uses -- all three channels in one 32-bit multiply, relying on the borrows
// between fields cancelling -- is the kind of thing a typo breaks for some
// colours only. So it is pinned against plain per-channel arithmetic.
static int FloorDiv32(int v) {
    return v >= 0 ? v / 32 : -((-v + 31) / 32);
}

static uint16_t RefBlend(uint16_t dst, uint16_t src, int alpha) {
    const int m = (alpha + 4) >> 3;
    const int sr = src >> 11, sg = (src >> 5) & 0x3F, sb = src & 0x1F;
    const int dr = dst >> 11, dg = (dst >> 5) & 0x3F, db = dst & 0x1F;
    const int r = dr + FloorDiv32((sr - dr) * m);
    const int g = dg + FloorDiv32((sg - dg) * m);
    const int b = db + FloorDiv32((sb - db) * m);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void test_blend_matches_channel_math(void) {
    int bad = 0;
    for (int i = 0; i < 200000; i++) {
        const uint16_t dst = (uint16_t)Rand();
        const uint16_t src = (uint16_t)Rand();
        const int alpha = (int)(Rand() % 256);
        const uint16_t want = RefBlend(dst, src, alpha);
        if (LineRaster_Blend(dst, src, (uint8_t)alpha, 0) != want) bad++;
        if (LineRaster_Blend(Swap16(dst), Swap16(src), (uint8_t)alpha, 1) != Swap16(want)) bad++;
    }
    check(bad == 0, "blend agrees with per-channel arithmetic, both byte orders");
    check(LineRaster_Blend(0x1234, 0xBEEF, 255, 0) == 0xBEEF, "alpha 255 is the source exactly");
    check(LineRaster_Blend(0x1234, 0xBEEF, 0, 0) == 0x1234, "alpha 0 is the destination exactly");
}

// ---------------------------------------------------------------------------
// Lines
// ---------------------------------------------------------------------------

// lv_draw_line's straight lines are crisp: exactly `width` full rows (or
// columns), placed by its w_half0/w_half1 split. The map's roads were drawn
// that way and a horizontal street must not turn soft.
static void test_axis_lines_are_crisp_and_where_lvgl_puts_them(void) {
    for (int32_t width = 1; width <= 8; width++) {
        const int32_t w = width - 1;
        const int32_t half0 = w >> 1;
        const int32_t half1 = half0 + (w & 1);

        Canvas c;
        CanvasInit(&c, 0, 0, 0, 0, BUF_W - 1, BUF_H - 1, 0);
        LineRaster_Line(&c.s, 20, 50, 100, 50, width, WHITE);
        int ok = 1;
        for (int32_t y = 30; y <= 70; y++) {
            const uint16_t px = c.pixels[y * BUF_W + 60]; // far from both ends
            const int full = y >= 50 - half0 && y <= 50 + half1;
            if (px != (full ? WHITE : BLACK)) ok = 0;
        }
        CanvasFree(&c);

        CanvasInit(&c, 0, 0, 0, 0, BUF_W - 1, BUF_H - 1, 0);
        LineRaster_Line(&c.s, 70, 10, 70, 120, width, WHITE);
        for (int32_t x = 50; x <= 90; x++) {
            const uint16_t px = c.pixels[60 * BUF_W + x];
            const int full = x >= 70 - half0 && x <= 70 + half1;
            if (px != (full ? WHITE : BLACK)) ok = 0;
        }
        CanvasFree(&c);

        char what[96];
        snprintf(what, sizeof(what), "width %d: horizontal and vertical lines are crisp", width);
        check(ok, what);
    }
}

static void test_random_lines_match_the_reference(void) {
    int bad_cases = 0;
    for (int i = 0; i < 1500; i++) {
        const int32_t ox = RandIn(-50, 50);
        const int32_t oy = RandIn(-50, 50);
        // A clip somewhere inside the buffer, sometimes all of it.
        int32_t cx1 = ox + RandIn(0, 40);
        int32_t cy1 = oy + RandIn(0, 40);
        int32_t cx2 = ox + BUF_W - 1 - RandIn(0, 40);
        int32_t cy2 = oy + BUF_H - 1 - RandIn(0, 40);
        if (i % 5 == 0) {
            cx1 = ox;
            cy1 = oy;
            cx2 = ox + BUF_W - 1;
            cy2 = oy + BUF_H - 1;
        }
        // Endpoints around and beyond the canvas, so lines cross the clip
        // edges, start outside it and miss it.
        const int32_t ax = ox + RandIn(-30, BUF_W + 30);
        const int32_t ay = oy + RandIn(-30, BUF_H + 30);
        int32_t bx;
        int32_t by;
        if (i % 3 == 0) {
            bx = ax + RandIn(-12, 12); // short, as most road segments are
            by = ay + RandIn(-12, 12);
        } else {
            bx = ox + RandIn(-30, BUF_W + 30);
            by = oy + RandIn(-30, BUF_H + 30);
        }
        const int32_t width = RandIn(1, LINE_RASTER_MAX_WIDTH);
        Canvas c;
        CanvasInit(&c, ox, oy, cx1, cy1, cx2, cy2, i & 1);
        if (DrawAndCompare(&c, ax, ay, bx, by, width, "random") != 0) bad_cases++;
        CanvasFree(&c);
    }
    check(bad_cases == 0, "1500 random lines, every pixel within tolerance, nothing outside clip");
}

// The road layer hands over segments whose ends were projected miles off the
// screen, clamped to the coordinate range. Only the part that crosses the
// strip may cost anything, and it must still be right.
static void test_long_lines_from_far_away(void) {
    int bad = 0;
    const int32_t far = 30000;
    const int32_t ends[][4] = {
        {-far, 70, far, 75},     // nearly horizontal across everything
        {80, -far, 85, far},     // nearly vertical
        {-far, -far, far, far},  // diagonal through the middle
        {-far, 200, far, -100},  // shallow, crossing
        {-far, -5, far, -5},     // horizontal, just above the clip
        {-far, 300, 10, far},    // misses entirely
    };
    for (size_t i = 0; i < sizeof(ends) / sizeof(ends[0]); i++) {
        for (int32_t width = 1; width <= 6; width++) {
            Canvas c;
            CanvasInit(&c, 0, 0, 10, 10, BUF_W - 11, BUF_H - 11, 1);
            bad += DrawAndCompare(&c, ends[i][0], ends[i][1], ends[i][2], ends[i][3], width,
                                  "far") != 0;
            CanvasFree(&c);
        }
    }
    check(bad == 0, "segments with far-off ends draw only, and exactly, what crosses the clip");
}

// Total ink should be the capsule's area, 2RL + pi R^2: a systematic error in
// the intervals shows up here as a road that is uniformly too heavy or too
// light, which a tolerance per pixel can let through.
static void test_ink_is_the_capsule_area(void) {
    const int32_t widths[] = {2, 3, 4, 6};
    int ok = 1;
    for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
        const int32_t width = widths[w];
        Canvas c;
        CanvasInit(&c, 0, 0, 0, 0, BUF_W - 1, BUF_H - 1, 0);
        LineRaster_Line(&c.s, 30, 40, 120, 95, width, WHITE);
        double ink = 0.0;
        for (int32_t i = 0; i < BUF_W * BUF_H; i++) {
            ink += (double)((c.pixels[i] >> 5) & 0x3F) / 63.0; // green carries the most bits
        }
        CanvasFree(&c);
        const double len = sqrt(90.0 * 90.0 + 55.0 * 55.0);
        const double r = width * 0.5;
        const double area = 2.0 * r * len + M_PI * r * r;
        if (fabs(ink - area) > area * 0.03) {
            printf("  width %d: ink %.1f, capsule %.1f\n", width, ink, area);
            ok = 0;
        }
    }
    check(ok, "ink matches the capsule's area to 3%");
}

static void test_refusals_and_points(void) {
    Canvas c;
    CanvasInit(&c, 0, 0, 0, 0, BUF_W - 1, BUF_H - 1, 0);
    check(!LineRaster_Line(&c.s, 10, 10, 50, 50, 0, WHITE), "width 0 is refused");
    check(!LineRaster_Line(&c.s, 10, 10, 50, 50, LINE_RASTER_MAX_WIDTH + 1, WHITE),
          "a width past the tables is refused, not drawn thinner");
    check(LineRaster_Line(&c.s, 40, 40, 40, 40, 4, WHITE), "a zero-length line is accepted");
    int untouched = 1;
    for (int32_t i = 0; i < BUF_W * BUF_H; i++) {
        if (c.pixels[i] != BLACK) untouched = 0;
    }
    check(untouched, "...and neither refusal nor point drew anything, as lv_draw_line");
    CanvasFree(&c);
}

// Four table slots against six widths in rotation: every draw after the first
// round finds its table evicted, and must rebuild it rather than use another
// width's.
static void test_table_eviction(void) {
    int bad = 0;
    for (int round = 0; round < 3; round++) {
        for (int32_t width = 1; width <= 6; width++) {
            Canvas c;
            CanvasInit(&c, 0, 0, 0, 0, BUF_W - 1, BUF_H - 1, 0);
            bad += DrawAndCompare(&c, 15, 20 + width * 10, 140, 60 + width * 7, width, "evict") != 0;
            CanvasFree(&c);
        }
    }
    check(bad == 0, "widths cycling through more slots than there are still draw right");
}

static void test_span_clips(void) {
    Canvas c;
    CanvasInit(&c, 5, 5, 20, 20, 60, 60, 0);
    LineRaster_Span(&c.s, 30, -100, 300, WHITE);
    LineRaster_Span(&c.s, 10, 0, 100, WHITE); // above the clip
    int ok = 1;
    for (int32_t y = 0; y < BUF_H; y++) {
        for (int32_t x = 0; x < BUF_W; x++) {
            const int32_t sx = x + 5;
            const int32_t sy = y + 5;
            const int inside = sx >= 20 && sx <= 60 && sy >= 20 && sy <= 60;
            const uint16_t want = !inside ? SENTINEL : (sy == 30 ? WHITE : BLACK);
            if (c.pixels[y * BUF_W + x] != want) ok = 0;
        }
    }
    check(ok, "a span fills its row within the clip and nothing else");
    CanvasFree(&c);
}

int main(void) {
    printf("LineRaster host tests\n");
    test_blend_matches_channel_math();
    test_axis_lines_are_crisp_and_where_lvgl_puts_them();
    test_random_lines_match_the_reference();
    test_long_lines_from_far_away();
    test_ink_is_the_capsule_area();
    test_refusals_and_points();
    test_table_eviction();
    test_span_clips();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
