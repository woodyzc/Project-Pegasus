#include "LineRaster.h"

#include <math.h>
#include <stddef.h>

// Whole pixels from an axis that any width this file takes can reach: a width
// of LINE_RASTER_MAX_WIDTH reaches R + 0.5 = 6.5 pixels.
#define TABLE_N (LINE_RASTER_MAX_WIDTH / 2 + 1)

// Stands in for "no limit" on one side of an interval. Clamped to the clip
// area before anything is converted to an integer.
#define BIG 1.0e30f

// Everything about a width that does not depend on the line's direction.
//
// The end discs are the only part of a capsule that needs a square root, and
// both their questions -- how wide is the disc at this row, and how much of
// this pixel does it cover -- only ever ask about whole-pixel offsets from an
// end point, because every end point sits on the pixel grid. So the answers
// are a few small tables per width, built once, and the per-row and per-pixel
// work is lookups.
typedef struct {
    int32_t width; // 0: slot not built yet
    float off;     // where the axis sits within a pixel: 0, or 0.5 for an even width
    float r_out;   // R + 0.5: a pixel centre this far out gets no coverage
    float r_in;    // R - 0.5: one this close in gets all of it
    int32_t k_max; // largest index any table below answers for
    // Half the chord that a disc of radius r_out (r_in) cuts from the row
    // k + off away from its centre. Negative where the row misses the disc.
    float chord_out[TABLE_N];
    float chord_in[TABLE_N];
    // Coverage, 0..255, of the pixel whose centre is (i + off, j + off) from
    // an end point.
    uint8_t cap[TABLE_N][TABLE_N];
} WidthTable_t;

// Four, because a frame of the map draws four widths: the roads' 2, 3 and 4,
// and the route's 6. The chevrons and the rider's outline are 2 again.
#define TABLE_SLOTS 4
static WidthTable_t s_tables[TABLE_SLOTS];
static uint8_t s_next_slot = 0;

static inline uint8_t Alpha(float coverage) {
    if (coverage >= 1.0f) {
        return 255;
    }
    if (coverage <= 0.0f) {
        return 0;
    }
    return (uint8_t)(coverage * 255.0f + 0.5f);
}

// Which table entry a whole-pixel offset from an end point lands on. With the
// axis half a pixel along, offsets 0 and 1 are both half a pixel from it, so
// they share entry 0.
static inline int32_t TableIndex(int32_t delta, int half) {
    if (half) {
        return delta >= 1 ? delta - 1 : -delta;
    }
    return delta >= 0 ? delta : -delta;
}

static void BuildTable(WidthTable_t *t, int32_t width) {
    const float r = (float)width * 0.5f;
    t->width = width;
    t->off = (width & 1) ? 0.0f : 0.5f;
    t->r_out = r + 0.5f;
    t->r_in = r - 0.5f;
    t->k_max = (int32_t)(t->r_out - t->off); // both positive, so this is a floor
    for (int k = 0; k < TABLE_N; k++) {
        const float m = (float)k + t->off;
        t->chord_out[k] = (m <= t->r_out) ? sqrtf((t->r_out * t->r_out) - (m * m)) : -1.0f;
        t->chord_in[k] =
            (t->r_in > 0.0f && m <= t->r_in) ? sqrtf((t->r_in * t->r_in) - (m * m)) : -1.0f;
        for (int j = 0; j < TABLE_N; j++) {
            const float n = (float)j + t->off;
            t->cap[k][j] = Alpha(t->r_out - sqrtf((m * m) + (n * n)));
        }
    }
}

static const WidthTable_t *TableFor(int32_t width) {
    for (int i = 0; i < TABLE_SLOTS; i++) {
        if (s_tables[i].width == width) {
            return &s_tables[i];
        }
    }
    WidthTable_t *t = &s_tables[s_next_slot];
    s_next_slot = (uint8_t)((s_next_slot + 1) % TABLE_SLOTS);
    BuildTable(t, width);
    return t;
}

// Floor and ceiling of a value already known to be in int32 range. Written
// out because floorf and ceilf are library calls on the ESP32-S3, and these
// run for every row.
static inline int32_t FloorInt(float v) {
    const int32_t i = (int32_t)v; // truncates towards zero
    return ((float)i > v) ? i - 1 : i;
}

static inline int32_t CeilInt(float v) {
    const int32_t i = (int32_t)v;
    return ((float)i < v) ? i + 1 : i;
}

// The pixels whose centres lie within [lo, hi], clipped to [x1, x2]. Empty
// when *first > *last. The comparisons come first so that an unbounded side
// never reaches the integer conversion.
static inline void PixelRange(float lo, float hi, int32_t x1, int32_t x2, int32_t *first,
                              int32_t *last) {
    *first = (lo <= (float)x1) ? x1 : (lo > (float)x2) ? x2 + 1 : CeilInt(lo);
    *last = (hi >= (float)x2) ? x2 : (hi < (float)x1) ? x1 - 1 : FloorInt(hi);
}

static inline void Hull(float *lo, float *hi, float a, float b) {
    if (a < *lo) {
        *lo = a;
    }
    if (b > *hi) {
        *hi = b;
    }
}

static inline uint16_t Swap16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

// lv_color_mix's own 16-bit path, for LV_COLOR_MIX_ROUND_OFS == 0: alpha
// reduced to 0..32, all three channels blended at once in one 32-bit word.
// Native byte order in and out.
static inline uint16_t MixNative(uint16_t fg, uint16_t bg, uint32_t alpha) {
    const uint32_t mix = (alpha + 4u) >> 3;
    const uint32_t b = ((uint32_t)bg | ((uint32_t)bg << 16)) & 0x7E0F81Fu;
    const uint32_t f = ((uint32_t)fg | ((uint32_t)fg << 16)) & 0x7E0F81Fu;
    const uint32_t r = ((((f - b) * mix) >> 5) + b) & 0x7E0F81Fu;
    return (uint16_t)((r >> 16) | r);
}

uint16_t LineRaster_Blend(uint16_t dst, uint16_t src, uint8_t alpha, bool swapped) {
    if (!swapped) {
        return MixNative(src, dst, alpha);
    }
    return Swap16(MixNative(Swap16(src), Swap16(dst), alpha));
}

static inline void Plot(uint16_t *px, uint8_t alpha, uint16_t colour, uint16_t native,
                        bool swapped) {
    if (alpha == 0) {
        return;
    }
    if (alpha == 255) {
        *px = colour;
        return;
    }
    if (swapped) {
        *px = Swap16(MixNative(native, Swap16(*px), alpha));
    } else {
        *px = MixNative(native, *px, alpha);
    }
}

void LineRaster_Span(const LineRasterSurface_t *s, int32_t y, int32_t x1, int32_t x2,
                     uint16_t colour) {
    if (s == NULL || s->pixels == NULL || y < s->clip_y1 || y > s->clip_y2) {
        return;
    }
    if (x1 < s->clip_x1) {
        x1 = s->clip_x1;
    }
    if (x2 > s->clip_x2) {
        x2 = s->clip_x2;
    }
    uint16_t *row = s->pixels + ((size_t)(y - s->origin_y) * (size_t)s->stride);
    for (int32_t x = x1; x <= x2; x++) {
        row[x - s->origin_x] = colour;
    }
}

bool LineRaster_Line(const LineRasterSurface_t *s, int32_t ax, int32_t ay, int32_t bx,
                     int32_t by, int32_t width, uint16_t colour) {
    if (s == NULL || s->pixels == NULL || width < 1 || width > LINE_RASTER_MAX_WIDTH) {
        return false;
    }
    if (ax == bx && ay == by) {
        return true; // as lv_draw_line: a point is not a line
    }

    // Rows and columns the outer edge can reach, against the clip. Anything
    // that misses it goes no further.
    const WidthTable_t *t = TableFor(width);
    const int32_t reach = t->k_max + 1;
    int32_t y1 = (ay < by ? ay : by) - reach;
    int32_t y2 = (ay > by ? ay : by) + reach;
    if (y1 < s->clip_y1) {
        y1 = s->clip_y1;
    }
    if (y2 > s->clip_y2) {
        y2 = s->clip_y2;
    }
    if (y1 > y2) {
        return true;
    }
    if ((ax > bx ? ax : bx) + reach < s->clip_x1 || (ax < bx ? ax : bx) - reach > s->clip_x2) {
        return true;
    }

    const int half = (width & 1) == 0;
    const float r_out = t->r_out;
    const float r_in = t->r_in;
    const int32_t k_max = t->k_max;

    // The axis, from A along u for len pixels; n is its normal. Measured from
    // pixel centres, which are the integers, so an even width's axis sits on
    // the boundary between two of them.
    const float a_x = (float)ax + t->off;
    const float a_y = (float)ay + t->off;
    const float dx = (float)(bx - ax);
    const float dy = (float)(by - ay);
    const float len = sqrtf((dx * dx) + (dy * dy));
    const float ux = dx / len;
    const float uy = dy / len;
    const float nx = -uy;
    const float ny = ux;
    // Zero exactly when the line is vertical (horizontal), which is when the
    // slab below stops depending on x and is handled as all-or-nothing.
    const float inv_ux = (bx != ax) ? 1.0f / ux : 0.0f;
    const float inv_nx = (by != ay) ? 1.0f / nx : 0.0f;

    const uint16_t native = s->swapped ? Swap16(colour) : colour;
    const int32_t ox = s->origin_x;

    for (int32_t y = y1; y <= y2; y++) {
        // Along a row, both the distance along the axis (t) and the signed
        // distance across it (sd) are linear in x: t = ux (x - a_x) + uyd and
        // sd = nx (x - a_x) + nyd.
        const float dyc = (float)y - a_y;
        const float uyd = uy * dyc;
        const float nyd = ny * dyc;

        // The row's interval of the capsule widened by half a pixel (outer:
        // any coverage at all) and narrowed by half a pixel (inner: full
        // coverage). Each is the hull of the same three pieces.
        float lo_o = BIG;
        float hi_o = -BIG;
        float lo_i = BIG;
        float hi_i = -BIG;

        // The end discs.
        const int32_t ka = TableIndex(y - ay, half);
        if (ka <= k_max) {
            if (t->chord_out[ka] >= 0.0f) {
                Hull(&lo_o, &hi_o, a_x - t->chord_out[ka], a_x + t->chord_out[ka]);
            }
            if (t->chord_in[ka] >= 0.0f) {
                Hull(&lo_i, &hi_i, a_x - t->chord_in[ka], a_x + t->chord_in[ka]);
            }
        }
        const int32_t kb = TableIndex(y - by, half);
        if (kb <= k_max) {
            const float b_x = a_x + dx;
            if (t->chord_out[kb] >= 0.0f) {
                Hull(&lo_o, &hi_o, b_x - t->chord_out[kb], b_x + t->chord_out[kb]);
            }
            if (t->chord_in[kb] >= 0.0f) {
                Hull(&lo_i, &hi_i, b_x - t->chord_in[kb], b_x + t->chord_in[kb]);
            }
        }

        // The slab between them: 0 <= t <= len and |sd| <= r, as offsets
        // from a_x.
        float t_lo = -BIG;
        float t_hi = BIG;
        int in_reach = 1;
        if (bx != ax) {
            const float p = -uyd * inv_ux;
            const float q = (len - uyd) * inv_ux;
            t_lo = p < q ? p : q;
            t_hi = p < q ? q : p;
        } else if (uyd < 0.0f || uyd > len) {
            in_reach = 0; // vertical, and this row is past one end
        }
        if (in_reach) {
            for (int pass = 0; pass < 2; pass++) {
                const float r = pass == 0 ? r_out : r_in;
                if (r <= 0.0f) {
                    continue;
                }
                float s_lo = -BIG;
                float s_hi = BIG;
                if (by != ay) {
                    const float p = (-r - nyd) * inv_nx;
                    const float q = (r - nyd) * inv_nx;
                    s_lo = p < q ? p : q;
                    s_hi = p < q ? q : p;
                } else if (nyd < -r || nyd > r) {
                    continue; // horizontal, and this row is outside it
                }
                const float lo = s_lo > t_lo ? s_lo : t_lo;
                const float hi = s_hi < t_hi ? s_hi : t_hi;
                if (lo <= hi) {
                    if (pass == 0) {
                        Hull(&lo_o, &hi_o, a_x + lo, a_x + hi);
                    } else {
                        Hull(&lo_i, &hi_i, a_x + lo, a_x + hi);
                    }
                }
            }
        }

        int32_t xo1;
        int32_t xo2;
        PixelRange(lo_o, hi_o, s->clip_x1, s->clip_x2, &xo1, &xo2);
        if (xo1 > xo2) {
            continue;
        }
        int32_t xi1;
        int32_t xi2;
        PixelRange(lo_i, hi_i, xo1, xo2, &xi1, &xi2);

        uint16_t *row = s->pixels + ((size_t)(y - s->origin_y) * (size_t)s->stride);

        // Everything in the outer run and not in the inner one is fringe,
        // and gets a coverage of its own. At most two runs: left and right.
        int32_t fringe_from[2];
        int32_t fringe_to[2];
        int fringes = 0;
        if (xi1 <= xi2) {
            fringe_from[0] = xo1;
            fringe_to[0] = xi1 - 1;
            fringe_from[1] = xi2 + 1;
            fringe_to[1] = xo2;
            fringes = 2;
            for (int32_t x = xi1; x <= xi2; x++) {
                row[x - ox] = colour;
            }
        } else {
            fringe_from[0] = xo1;
            fringe_to[0] = xo2;
            fringes = 1;
        }

        for (int f = 0; f < fringes; f++) {
            if (fringe_from[f] > fringe_to[f]) {
                continue;
            }
            const float px = (float)fringe_from[f] - a_x;
            float along = (ux * px) + uyd;
            float across = (nx * px) + nyd;
            for (int32_t x = fringe_from[f]; x <= fringe_to[f]; x++) {
                uint8_t alpha;
                if (along <= 0.0f) {
                    const int32_t ki = TableIndex(x - ax, half);
                    alpha = (ki <= k_max && ka <= k_max) ? t->cap[ki][ka] : 0;
                } else if (along >= len) {
                    const int32_t ki = TableIndex(x - bx, half);
                    alpha = (ki <= k_max && kb <= k_max) ? t->cap[ki][kb] : 0;
                } else {
                    alpha = Alpha(r_out - fabsf(across));
                }
                Plot(&row[x - ox], alpha, colour, native, s->swapped);
                along += ux;
                across += nx;
            }
        }
    }
    return true;
}
