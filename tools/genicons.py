#!/usr/bin/env python3
"""Draw the turn-by-turn maneuver arrows and emit them as LVGL image data.

LVGL's symbol font has no maneuver arrows -- only chevrons -- so the head unit
was showing a bare "<" for a left turn. These are proper arrows: a shaft that
bends and ends in a filled head, the way a bike computer draws them.

Rendered at 4x and downsampled, which is where the antialiasing comes from.
Output is LV_IMG_CF_ALPHA_8BIT: one byte of coverage per pixel, no colour.
The colour comes from lv_obj_set_style_img_recolor() at runtime, so the same
asset can be drawn in the accent colour or greyed out when a route ends.

    python genicons.py <out.cpp> <preview.png>

Output must be .cpp, not .c: TbtIcons.h pulls in DataCenter.h for the
TBT_ICON_* enum, and DataCenter.h declares a C++ class.
"""
import sys
import math
from PIL import Image, ImageDraw

SIZE = 112        # final icon, px
SS = 4             # supersample factor
W = SIZE * SS
# Proportional to the icon, NOT absolute pixels. They were absolute, and when
# SIZE went from 64 to 112 the stroke stayed 8px while everything around it
# grew -- so the arrows came out spindly. Anything sized here must scale with W.
SHAFT = int(W * 0.165)   # stroke width
HEAD = int(W * 0.25)     # arrowhead length along its own axis
HALF = int(W * 0.215)    # arrowhead half-width, so the head is ~2.6x the shaft

# Icon ids from DataCenter.h. Order matters: the generated table is indexed by
# TBT_ICON_* directly, so a gap here would silently shift every icon after it.
ICONS = [
    "NONE", "STRAIGHT", "TURN_LEFT", "TURN_RIGHT", "SLIGHT_LEFT",
    "SLIGHT_RIGHT", "SHARP_LEFT", "SHARP_RIGHT", "UTURN", "ROUNDABOUT",
    "ARRIVE",
]


def _cap(d, p):
    r = SHAFT // 2
    d.ellipse([p[0] - r, p[1] - r, p[0] + r, p[1] + r], fill=255)


def arrow(d, pts):
    """Polyline ending in a filled head. `pts` runs to the TIP.

    The last segment is shortened by the head length before stroking, so the
    shaft stops where the head begins instead of being buried inside it --
    which is what turned the first attempt into a row of blobs.
    """
    tip = pts[-1]
    prev = pts[-2]
    dx, dy = tip[0] - prev[0], tip[1] - prev[1]
    seg = math.hypot(dx, dy)
    ux, uy = dx / seg, dy / seg
    base = (tip[0] - HEAD * ux, tip[1] - HEAD * uy)

    body = list(pts[:-1]) + [base]
    if len(body) >= 2:
        d.line(body, fill=255, width=SHAFT, joint="curve")
        for p in body[:-1]:
            _cap(d, p)

    # Head: tip plus two corners square to the direction of travel.
    lx, ly = -uy, ux
    d.polygon([
        tip,
        (base[0] + HALF * lx, base[1] + HALF * ly),
        (base[0] - HALF * lx, base[1] - HALF * ly),
    ], fill=255)


def shaft(d, pts):
    d.line(pts, fill=255, width=SHAFT, joint="curve")
    for p in pts:
        _cap(d, p)


def draw(name):
    img = Image.new("L", (W, W), 0)
    d = ImageDraw.Draw(img)
    c = W // 2
    bot = int(W * 0.88)

    if name == "STRAIGHT":
        arrow(d, [(c, bot), (c, int(W * 0.10))])

    elif name in ("TURN_LEFT", "TURN_RIGHT"):
        s = -1 if name.endswith("LEFT") else 1
        bend = int(W * 0.40)
        arrow(d, [(c, bot), (c, bend), (c + s * int(W * 0.38), bend)])

    elif name in ("SLIGHT_LEFT", "SLIGHT_RIGHT"):
        s = -1 if name.endswith("LEFT") else 1
        # Bend late and leave at 45 degrees: the difference from a plain turn
        # has to be legible at a glance on a bouncing bike.
        bend = int(W * 0.62)
        run = int(W * 0.34)
        arrow(d, [(c, bot), (c, bend), (c + s * run, bend - run)])

    elif name in ("SHARP_LEFT", "SHARP_RIGHT"):
        s = -1 if name.endswith("LEFT") else 1
        # Up, a short jog sideways, then back DOWN: the doubling-back is what
        # separates this from a slight turn without needing a label.
        #
        # The jog is not decoration. Turning back straight from the shaft put
        # a head corner on top of the shaft itself and the whole icon merged
        # into a blob -- the head is 2.7x the stroke width, so it needs to
        # start clear of it.
        top = int(W * 0.24)
        jog = int(W * 0.36)
        arrow(d, [(c, bot), (c, top), (c + s * jog, top),
                  (c + s * jog, int(W * 0.72))])

    elif name == "UTURN":
        r = int(W * 0.17)
        right = c + r
        left = c - r
        top = int(W * 0.30)
        shaft(d, [(right, bot), (right, top)])
        d.arc([left, top - r, right, top + r], 180, 360, fill=255, width=SHAFT)
        arrow(d, [(left, top), (left, int(W * 0.74))])

    elif name == "ROUNDABOUT":
        # Entry from the bottom, around the island, exit right. The exit
        # leaves from the top of the circle rather than its side so the
        # arrow does not grow out of the ring like a lollipop stick.
        # Radius has to clear the stroke: at r=0.21W the ring width (0.165W)
        # nearly closed the hole and the island read as a solid blob.
        r = int(W * 0.29)
        cy = int(W * 0.40)
        d.ellipse([c - r, cy - r, c + r, cy + r], outline=255, width=SHAFT)
        shaft(d, [(c, bot), (c, cy + r)])
        arrow(d, [(c + int(r * 0.70), cy - int(r * 0.70)),
                  (c + int(W * 0.42), cy - int(W * 0.32))])

    elif name == "ARRIVE":
        # Chequered flag, the same idea the Coospo uses for a destination.
        pole = int(W * 0.26)
        top = int(W * 0.16)
        cell = int(W * 0.11)
        shaft(d, [(pole, bot), (pole, top)])
        for row in range(3):
            for col in range(4):
                if (row + col) % 2:
                    continue
                x0 = pole + col * cell
                y0 = top + row * cell
                d.rectangle([x0, y0, x0 + cell, y0 + cell], fill=255)
        d.rectangle([pole, top, pole + 4 * cell, top + 3 * cell], outline=255, width=SS * 2)

    # NONE renders empty on purpose: the dashboard shows "NO ROUTE" instead.
    return img.resize((SIZE, SIZE), Image.LANCZOS)


def emit(f, name, img):
    px = list(img.getdata())
    f.write(f"// {name}\n")
    f.write(f"static const uint8_t tbt_icon_{name.lower()}_map[] = {{\n")
    for i in range(0, len(px), 16):
        row = ", ".join(f"0x{v:02x}" for v in px[i:i + 16])
        f.write(f"    {row},\n")
    f.write("};\n")
    f.write(f"static const lv_img_dsc_t tbt_icon_{name.lower()} = {{\n")
    f.write("    .header = {.cf = LV_IMG_CF_ALPHA_8BIT, .always_zero = 0, .reserved = 0,\n")
    f.write(f"               .w = {SIZE}, .h = {SIZE}}},\n")
    f.write(f"    .data_size = sizeof(tbt_icon_{name.lower()}_map),\n")
    f.write(f"    .data = tbt_icon_{name.lower()}_map,\n")
    f.write("};\n\n")


def main():
    out_c, out_png = sys.argv[1], sys.argv[2]
    imgs = [(n, draw(n)) for n in ICONS]

    with open(out_c, "w") as f:
        f.write("// GENERATED by tools/genicons.py -- do not edit by hand.\n")
        f.write("// Regenerate: python tools/genicons.py src/ui/TbtIcons.cpp /tmp/preview.png\n")
        f.write("//\n")
        f.write("// Turn-by-turn maneuver arrows. LVGL's symbol font carries chevrons and\n")
        f.write("// arrows but no maneuver glyphs, so a left turn used to render as a bare\n")
        f.write('// "<". These are drawn as real arrows: a shaft that bends into a filled\n')
        f.write("// head.\n")
        f.write("//\n")
        f.write("// LV_IMG_CF_ALPHA_8BIT: one coverage byte per pixel and no colour of its\n")
        f.write("// own, so lv_obj_set_style_img_recolor() picks the colour at runtime and\n")
        f.write("// one asset serves both the live and greyed-out states. Alpha images draw\n")
        f.write("// through the normal mask path and do NOT need LV_COLOR_SCREEN_TRANSP,\n")
        f.write("// which is the flag that made lv_canvas panic the board.\n\n")
        f.write('#include "TbtIcons.h"\n\n')
        for n, im in imgs:
            emit(f, n, im)
        f.write("const lv_img_dsc_t *TbtIcon(uint8_t icon_id) {\n")
        f.write("    switch (icon_id) {\n")
        for n in ICONS:
            if n == "NONE":
                continue
            f.write(f"        case TBT_ICON_{n}: return &tbt_icon_{n.lower()};\n")
        f.write("        case TBT_ICON_NONE:\n")
        f.write("        default: return &tbt_icon_straight;\n")
        f.write("    }\n}\n")

    # Preview sheet, white on the dashboard's own background so it is judged
    # in the colours it will actually appear in.
    pad = 10
    sheet = Image.new("RGB", (len(imgs) * (SIZE + pad) + pad, SIZE + 2 * pad), (0x10, 0x18, 0x20))
    for i, (n, im) in enumerate(imgs):
        tint = Image.new("RGB", (SIZE, SIZE), (0x61, 0xDA, 0xFB))
        sheet.paste(tint, (pad + i * (SIZE + pad), pad), im)
    sheet = sheet.resize((sheet.width * 2, sheet.height * 2), Image.NEAREST)
    sheet.save(out_png)
    print(f"{len(imgs)} icons -> {out_c}")
    print(f"preview -> {out_png}")
    print(f"flash cost: {len(imgs) * SIZE * SIZE / 1024:.0f} KB")


if __name__ == "__main__":
    main()
