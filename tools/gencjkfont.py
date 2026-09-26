#!/usr/bin/env python3
"""Generate an LVGL font that can draw a Chinese name.

Every font in this firmware is Montserrat, which covers 0x20-0x7F and nothing
else -- so a WeChat message from 张三 arrives intact over BLE, parses cleanly,
and renders as an empty banner. This closes that gap for the one label that
needs it.

    python tools/gencjkfont.py <out.c> <NotoSansCJKsc-Regular.otf> <size_px>

The typeface is **Noto Sans CJK SC**, under the SIL Open Font License, which
is what makes embedding rendered glyphs in a firmware image allowed. macOS
ships PingFang and STHeiti and both are proprietary, so neither can go in a
repository. The OTF is 16MB and is NOT vendored -- fetch it when regenerating:

    https://github.com/notofonts/noto-cjk/raw/main/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf

---------------------------------------------------------------------------
Which characters, and why not all of them
---------------------------------------------------------------------------
Everything encodable in **GB2312**: 6,763 hanzi plus 533 punctuation and
fullwidth forms, plus ASCII so a mixed name like "Alex 张" draws from one
font -- LVGL cannot switch faces inside a label.

The obvious alternative, the whole contiguous 0x4E00-0x9FFF block, is 20,992
glyphs. That is not merely wasteful, it does not work: LVGL's
`lv_font_fmt_txt_glyph_dsc_t` packs `bitmap_index` into **20 bits** when
`LV_FONT_FMT_TXT_LARGE` is 0, which it is here, so a font's bitmap data cannot
exceed 1MB. The full block at this size and depth is past that, and the
overflow is silent -- glyphs beyond 1MB would draw as garbage from a wrapped
index, with no warning at build time. This script refuses to emit such a font;
see the check in main().

---------------------------------------------------------------------------
Two bits per pixel
---------------------------------------------------------------------------
Not one. A 20px hanzi is a dozen strokes inside 20 pixels, and without any
antialiasing the thin ones drop out or alias into each other -- legibility at
a glance, on a bike, is the entire point of putting the name on screen. 4bpp
would be better again and is ruled out by the 1MB ceiling above, not by taste.

LVGL maps 2bpp through `_lv_bpp2_opa_table` = {0, 85, 170, 255}, a linear
ramp, so the quantisation here is a plain round.

Bits are packed MSB first as ONE CONTINUOUS STREAM per glyph -- rows are not
padded to byte boundaries. `lv_draw_sw_letter.c` computes
`bit_ofs = row * (box_w * bpp) + col * bpp`, so a per-row alignment would
shear every glyph diagonally. Only the start of each glyph is byte-aligned,
because `bitmap_index` is a byte index.
"""
import sys

from PIL import Image, ImageDraw, ImageFont

BPP = 2
PAD = 8  # canvas margin, so no glyph is clipped before it is measured

# ASCII gets its own contiguous character map. It is the cheapest kind LVGL
# has -- glyph_id = start + (codepoint - range_start), no search at all -- and
# keeping it separate also stops the sparse range below from having to span
# 0x20 to 0xFFE5, whose relative codepoints would come close to overflowing
# the uint16 the sparse list is made of.
ASCII_FIRST = 0x20
ASCII_LAST = 0x7E

# Common in Chinese text and absent from GB2312, which is a 1980 standard.
# Each of these was found missing by rendering it -- see test/sim/fontproof.cpp.
EXTRAS = [
    0x00A0,  # no-break space
    0x00B7,  # middle dot. The important one: it is what separates the parts of
             # a transliterated foreign name, so a contact saved as
             # 玛丽·史密斯 loses its separator without it.
    0x2014,  # em dash. GB2312's A1AA decodes to U+2015 HORIZONTAL BAR in
             # Python's codec, so the dash people actually type is not in the
             # set that produced this charset.
    0x2022,  # bullet
]

# Emoji are deliberately out. They are outside the BMP, so they would not fit
# the uint16 the sparse character map searches, and they are colour glyphs
# besides. A name containing one draws a box where it stands, which is honest.


def charset():
    """ASCII, then everything else GB2312 can encode, each sorted."""
    ascii_cps = list(range(ASCII_FIRST, ASCII_LAST + 1))
    wide = []
    for cp in range(ASCII_LAST + 1, 0x10000):
        try:
            chr(cp).encode("gb2312")
        except (UnicodeEncodeError, UnicodeError):
            continue
        wide.append(cp)
    wide = sorted(set(wide) | set(EXTRAS))
    return ascii_cps, wide


def render(font, ch, ascent):
    """One glyph as (packed bytes, box_w, box_h, ofs_x, ofs_y, adv_w)."""
    adv_w = int(round(font.getlength(ch) * 16))  # 1/16 px, LVGL's 8.4 format

    height = ascent + font.getmetrics()[1] + 2 * PAD
    width = int(font.getlength(ch)) + 4 * PAD
    img = Image.new("L", (width, height), 0)
    draw = ImageDraw.Draw(img)
    # "ls" anchors at the left edge on the BASELINE, which is the origin LVGL
    # measures its offsets from too.
    draw.text((PAD, PAD + ascent), ch, font=font, fill=255, anchor="ls")

    box = img.getbbox()
    if box is None:  # a space, or a codepoint this face has no glyph for
        return b"", 0, 0, 0, 0, adv_w

    x0, y0, x1, y1 = box
    crop = img.crop(box)
    w, h = x1 - x0, y1 - y0

    # ofs_y is the baseline-to-box-bottom distance, positive upwards: a glyph
    # sitting on the baseline gives 0, one hanging below gives a negative.
    ofs_y = (PAD + ascent) - y1

    # Pack MSB first, continuously across rows. See the module docstring.
    bits = bytearray()
    acc = 0
    nbits = 0
    px = crop.load()
    for row in range(h):
        for col in range(w):
            value = (px[col, row] * 3 + 127) // 255  # round to 0..3
            acc = (acc << BPP) | value
            nbits += BPP
            if nbits == 8:
                bits.append(acc & 0xFF)
                acc = 0
                nbits = 0
    if nbits:  # last byte of the glyph, zero-padded
        bits.append((acc << (8 - nbits)) & 0xFF)

    return bytes(bits), w, h, x0 - PAD, ofs_y, adv_w


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    out_c, ttf, size = sys.argv[1], sys.argv[2], int(sys.argv[3])

    font = ImageFont.truetype(ttf, size)
    ascent, descent = font.getmetrics()

    ascii_cps, wide_cps = charset()
    all_cps = ascii_cps + wide_cps

    blob = bytearray()
    dscs = []
    for cp in all_cps:
        data, w, h, ox, oy, adv = render(font, chr(cp), ascent)
        dscs.append((len(blob), adv, w, h, ox, oy))
        blob += data

    # The silent-corruption guard. LVGL stores bitmap_index in 20 bits when
    # LV_FONT_FMT_TXT_LARGE is 0, so anything past 1MB would wrap and draw
    # garbage with nothing failing at build time.
    LIMIT = 1 << 20
    if len(blob) >= LIMIT:
        sys.exit(f"bitmap is {len(blob)} bytes, over LVGL's {LIMIT}-byte "
                 f"limit for bitmap_index:20 -- reduce the size or the charset")

    # Relative codepoints for the sparse map, which searches uint16 values.
    wide_start = wide_cps[0]
    wide_span = wide_cps[-1] - wide_start
    assert wide_span <= 0xFFFF, "sparse range does not fit a uint16 relative codepoint"

    name = f"pegasus_font_cjk_{size}"
    with open(out_c, "w") as f:
        f.write("// GENERATED by tools/gencjkfont.py -- do not edit by hand.\n")
        f.write(f"// Regenerate: python tools/gencjkfont.py {out_c} "
                "<NotoSansCJKsc-Regular.otf> " f"{size}\n")
        f.write("//\n")
        f.write("// Noto Sans CJK SC, GB2312 plus ASCII, 2 bits of coverage per pixel.\n")
        f.write("// The typeface is under the SIL Open Font License, which is what\n")
        f.write("// permits these rendered glyphs to be embedded and redistributed.\n")
        f.write("// Upstream: https://github.com/notofonts/noto-cjk\n")
        f.write("//\n")
        f.write(f"// {len(all_cps)} glyphs, {len(blob) / 1024:.0f} KB of bitmap.\n\n")
        f.write('#include "lvgl.h"\n\n')

        f.write("static const uint8_t glyph_bitmap[] = {\n")
        for i in range(0, len(blob), 16):
            f.write("    " + ", ".join(f"0x{b:02x}" for b in blob[i:i + 16]) + ",\n")
        f.write("};\n\n")

        f.write("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {\n")
        f.write("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0,"
                " .ofs_x = 0, .ofs_y = 0}, /* id 0 is reserved: 'not found' */\n")
        for idx, adv, w, h, ox, oy in dscs:
            f.write(f"    {{.bitmap_index = {idx}, .adv_w = {adv}, .box_w = {w},"
                    f" .box_h = {h}, .ofs_x = {ox}, .ofs_y = {oy}}},\n")
        f.write("};\n\n")

        f.write("// Relative to the sparse range start, which is what LVGL searches.\n")
        f.write("static const uint16_t unicode_list[] = {\n")
        rels = [cp - wide_start for cp in wide_cps]
        for i in range(0, len(rels), 12):
            f.write("    " + ", ".join(str(r) for r in rels[i:i + 12]) + ",\n")
        f.write("};\n\n")

        f.write("static const lv_font_fmt_txt_cmap_t cmaps[] = {\n")
        f.write("    {\n")
        f.write(f"        .range_start = {ASCII_FIRST}, "
                f".range_length = {len(ascii_cps)}, .glyph_id_start = 1,\n")
        f.write("        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0,\n")
        f.write("        .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY\n")
        f.write("    },\n")
        f.write("    {\n")
        f.write(f"        .range_start = {wide_start}, .range_length = {wide_span + 1},\n")
        f.write(f"        .glyph_id_start = {len(ascii_cps) + 1},\n")
        f.write("        .unicode_list = unicode_list, .glyph_id_ofs_list = NULL,\n")
        f.write(f"        .list_length = {len(wide_cps)},\n")
        f.write("        .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY\n")
        f.write("    }\n};\n\n")

        f.write("static lv_font_fmt_txt_glyph_cache_t cache;\n")
        f.write("static const lv_font_fmt_txt_dsc_t font_dsc = {\n")
        f.write("    .glyph_bitmap = glyph_bitmap,\n")
        f.write("    .glyph_dsc = glyph_dsc,\n")
        f.write("    .cmaps = cmaps,\n")
        # No kerning. CJK is monospaced by design and the table would be tens
        # of kilobytes to nudge the Latin pairs in a name.
        f.write("    .kern_dsc = NULL,\n")
        f.write("    .kern_scale = 0,\n")
        f.write("    .cmap_num = 2,\n")
        f.write(f"    .bpp = {BPP},\n")
        f.write("    .kern_classes = 0,\n")
        f.write("    .bitmap_format = 0,\n")
        f.write("    .cache = &cache\n};\n\n")

        f.write(f"const lv_font_t {name} = {{\n")
        f.write("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,\n")
        f.write("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,\n")
        # The face's own metrics, not the glyphs' extent. gennumfont.py shrinks
        # the line box to the ink because it holds eleven digits that all sit
        # on the baseline; this face has thousands of glyphs and wraps to a
        # second line, so a line box measured from ink would let a tall glyph
        # on one line touch a tall one on the next.
        f.write(f"    .line_height = {ascent + descent},\n")
        f.write(f"    .base_line = {descent},\n")
        f.write("    .subpx = LV_FONT_SUBPX_NONE,\n")
        f.write(f"    .underline_position = {-max(1, size // 12)},\n")
        f.write(f"    .underline_thickness = {max(1, size // 24)},\n")
        f.write("    .dsc = &font_dsc\n};\n")

    print(f"{name}: {len(all_cps)} glyphs "
          f"({len(ascii_cps)} ascii + {len(wide_cps)} gb2312)")
    print(f"  bitmap  {len(blob) / 1024:8.1f} KB  ({100 * len(blob) / LIMIT:.0f}% of LVGL's 1MB cap)")
    print(f"  dsc     {len(dscs) * 8 / 1024:8.1f} KB")
    print(f"  cmap    {len(wide_cps) * 2 / 1024:8.1f} KB")
    print(f"  line_height={ascent + descent} base_line={descent}")


if __name__ == "__main__":
    main()
