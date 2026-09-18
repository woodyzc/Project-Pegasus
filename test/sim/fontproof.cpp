// Proves the generated CJK font actually draws, using the real LVGL.
//
// tools/gencjkfont.py packs glyph bitmaps by hand -- 2 bits per pixel, MSB
// first, as one continuous stream with no per-row padding, because that is
// what lv_draw_sw_letter.c unpacks. Every part of that sentence is a chance to
// be subtly wrong in a way that still compiles, still links, and produces
// plausible-looking garbage only visible on the panel. Two bits shifted and
// the strokes shear; a misread ofs_y and the line sits wrong; a bad cmap and
// the wrong character appears entirely.
//
// So it is rendered here first, through LVGL's own font lookup and its own
// glyph blitter, and looked at.
//
//   cd test/sim && make fontproof
//
// Not a pass/fail test -- there is no reference image to diff against, and
// the thing being checked is legibility, which is a judgement. It turns a
// flash cycle into a second.
#include <cstdio>
#include <cstring>

#include "lvgl.h"

#include "../../src/ui/CjkFont.h"

#define SCREEN_W 240
#define SCREEN_H 320

extern "C" unsigned int sim_millis(void) { return 0; }

static lv_color_t s_canvas[SCREEN_W * SCREEN_H];

static void FlushCb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *colors) {
    for (int32_t y = area->y1; y <= area->y2; y++) {
        for (int32_t x = area->x1; x <= area->x2; x++) {
            s_canvas[y * SCREEN_W + x] = *colors++;
        }
    }
    lv_disp_flush_ready(drv);
}

static void WritePpm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (f == nullptr) {
        fprintf(stderr, "cannot write %s\n", path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        const uint16_t c = s_canvas[i].full;
        const uint8_t rgb[3] = {
            (uint8_t)(((c >> 11) & 0x1F) * 255 / 31),
            (uint8_t)(((c >> 5) & 0x3F) * 255 / 63),
            (uint8_t)((c & 0x1F) * 255 / 31),
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

// Stacked by a flex column rather than at hand-computed y positions. The
// first version of this advanced y by a fixed amount per line, and the two
// lines that wrapped silently drew on top of the ones below -- which made the
// size comparison, the whole point of those lines, unreadable.
static void Line(lv_obj_t *parent, const char *text, const lv_font_t *font,
                 uint32_t colour) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(colour), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
}

int main(int argc, char **argv) {
    const char *out_dir = (argc > 1) ? argv[1] : "out";

    lv_init();

    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t buf[SCREEN_W * 40];
    lv_disp_draw_buf_init(&draw_buf, buf, nullptr, SCREEN_W * 40);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = SCREEN_W;
    disp_drv.ver_res = SCREEN_H;
    disp_drv.flush_cb = FlushCb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x18222C), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *column = lv_obj_create(scr);
    lv_obj_remove_style_all(column);
    lv_obj_set_pos(column, 12, 8);
    lv_obj_set_size(column, SCREEN_W - 24, SCREEN_H - 16);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(column, 4, 0);
    lv_obj_clear_flag(column, LV_OBJ_FLAG_SCROLLABLE);


    // The three things the banner puts on screen, in the fonts it uses.
    Line(column, "WeChat", &lv_font_montserrat_14, 0x2FC6B7);

    // A plain two-character name: the common case, and the one that has to be
    // unmistakable at a glance.
    Line(column, "\xE5\xBC\xA0\xE4\xB8\x89", &pegasus_font_cjk_20, 0xFFFFFF);

    // Mixed scripts in one label, which is the reason ASCII is in this font
    // at all -- LVGL cannot change face mid-string.
    Line(column, "Alex \xE5\xBC\xA0 2026", &pegasus_font_cjk_20, 0xFFFFFF);

    // Dense strokes. If the 2bpp quantisation or the bit packing is wrong,
    // these are where it shows first: they are the glyphs with the most ink
    // per pixel in the whole charset.
    Line(column, "\xE9\xBD\x89\xE9\xBE\x8C\xE8\xA1\x8C\xE8\x80\x80\xE9\xB9\xB0",
         &pegasus_font_cjk_20, 0xFFFFFF);

    // Punctuation and fullwidth forms, which come from the sparse half of the
    // character map at codepoints far from the hanzi.
    Line(column, "\xE3\x80\x8A\xE7\xBE\xA4\xE8\x81\x8A\xE3\x80\x8B\xEF\xBC\x9A"
                 "\xE4\xB8\x89\xE6\x9D\xA1",
         &pegasus_font_cjk_20, 0xFFFFFF);

    // A long group name wrapping to a second line. Checks line_height: too
    // small and the tall glyphs of one line touch the next.
    Line(column, "\xE5\x91\xA8\xE6\x9C\xAB\xE9\xAA\x91\xE8\xA1\x8C\xE7\xBE\xA4"
                 "\xE2\x80\x94\xE6\x98\x8E\xE5\xA4\xA9\xE4\xB8\x83\xE7\x82\xB9"
                 "\xE5\x87\xBA\xE5\x8F\x91\xE4\xB8\x8D\xE8\xA7\x81\xE4\xB8\x8D"
                 "\xE6\x95\xA3",
         &pegasus_font_cjk_20, 0xFFFFFF);

    // The four codepoints added to the charset by hand, because GB2312 is a
    // 1980 standard and does not have them. The middle dot is the one that
    // matters: it separates the parts of a transliterated foreign name.
    Line(column, "\xE7\x8E\x9B\xE4\xB8\xBD\xC2\xB7\xE5\x8F\xB2\xE5\xAF\x86"
                 "\xE6\x96\xAF \xE2\x80\x94 \xE2\x80\xA2",
         &pegasus_font_cjk_20, 0xFFFFFF);

    // ASCII in both faces, one above the other. The CJK face's Latin should
    // be legible and roughly the same size, or a mixed name will look broken.
    Line(column, "Montserrat 18: Handgloves 0123", &lv_font_montserrat_18, 0x93A4B8);
    Line(column, "Noto CJK 20: Handgloves 0123", &pegasus_font_cjk_20, 0x93A4B8);

    // A codepoint deliberately OUTSIDE the charset -- a rare hanzi GB2312
    // cannot encode. It must come out blank, not as some other character:
    // a cmap error shows up here as the wrong glyph rather than nothing.
    Line(column, "outside GB2312: [\xF0\xA0\x80\x80]", &pegasus_font_cjk_20, 0xFFD166);

    lv_refr_now(nullptr);

    char path[256];
    snprintf(path, sizeof(path), "%s/cjk-font-proof.ppm", out_dir);
    WritePpm(path);
    printf("wrote %s\n", path);
    return 0;
}
