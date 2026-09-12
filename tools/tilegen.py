#!/usr/bin/env python3
"""Map tiles for the head unit: convert, verify, and synthesise.

Step 1 of the offline-map scope. The device cannot decode PNG -- there is no
decoder in the firmware and no room to want one -- so tiles are converted to
LVGL's own raw format on a computer and the firmware just reads bytes.

    tilegen.py mapsim  OUT_DIR  [--n 3] [--seed 7] [--preview sheet.png]
    tilegen.py synth   OUT_DIR  [--zoom 15] [--x 8721] [--y 12556] [--n 3]
    tilegen.py convert IN.png   OUT.bin
    tilegen.py verify  IN.bin   [OUT.png]

`synth` is what the bring-up spike uses: it draws tiles that state their own
z/x/y and carry a hard border and a centre cross, so a misplaced or misordered
tile is obvious on sight rather than subtly wrong. No network, and no licensing
question to settle before the firmware can be tested.

ON REAL TILES, DELIBERATELY ABSENT
----------------------------------
There is no download command here, and that is not an oversight. OpenStreetMap's
public tile servers prohibit bulk downloading outright, and a scraper committed
to an open-source repository would be both blocked and indefensible. Real tiles
need either a provider whose terms permit offline caching, or a locally rendered
OSM extract. Whichever it is, it converts through `convert` above -- the format
work is done and does not depend on the answer.

FORMAT
------
LVGL v8 image file: a 4-byte header, then raw pixels.

    header u32, little-endian:  cf | always_zero<<5 | reserved<<8 | w<<10 | h<<21
    cf = 4 (LV_IMG_CF_TRUE_COLOR), matching LV_COLOR_DEPTH 16 in lv_conf.h
    pixels: RGB565 little-endian, row-major, no padding

256x256 therefore comes to 4 + 131072 = 131076 bytes. LV_COLOR_16_SWAP is 0 in
lv_conf.h, so the bytes are native order; flip that setting and every tile on
the card has to be regenerated.
"""
import argparse
import os
import struct
import sys

from PIL import Image, ImageDraw

TILE = 256
LV_IMG_CF_TRUE_COLOR = 4
HEADER_BYTES = 4


def lvgl_header(w: int, h: int, cf: int = LV_IMG_CF_TRUE_COLOR) -> bytes:
    if not (0 < w < 2048 and 0 < h < 2048):
        raise ValueError(f"{w}x{h} does not fit LVGL's 11-bit size fields")
    return struct.pack("<I", (cf & 0x1F) | ((w & 0x7FF) << 10) | ((h & 0x7FF) << 21))


def parse_header(blob: bytes):
    (v,) = struct.unpack("<I", blob[:HEADER_BYTES])
    return {
        "cf": v & 0x1F,
        "always_zero": (v >> 5) & 0x7,
        "reserved": (v >> 8) & 0x3,
        "w": (v >> 10) & 0x7FF,
        "h": (v >> 21) & 0x7FF,
    }


def to_rgb565(img: Image.Image) -> bytes:
    img = img.convert("RGB")
    out = bytearray()
    for r, g, b in img.getdata():
        out += struct.pack("<H", ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
    return bytes(out)


def from_rgb565(data: bytes, w: int, h: int) -> Image.Image:
    img = Image.new("RGB", (w, h))
    px = img.load()
    for i in range(w * h):
        (v,) = struct.unpack_from("<H", data, i * 2)
        # Replicate the high bits into the low ones so a round-trip lands on
        # the same value rather than drifting dark on every pass.
        r = ((v >> 11) & 0x1F) << 3
        g = ((v >> 5) & 0x3F) << 2
        b = (v & 0x1F) << 3
        px[i % w, i // w] = (r | (r >> 5), g | (g >> 6), b | (b >> 5))
    return img


def write_tile(path: str, img: Image.Image) -> int:
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    blob = lvgl_header(img.width, img.height) + to_rgb565(img)
    with open(path, "wb") as f:
        f.write(blob)
    return len(blob)


def cmd_convert(args) -> int:
    img = Image.open(args.src)
    if img.size != (TILE, TILE):
        print(f"note: {img.size[0]}x{img.size[1]}, not the usual {TILE}x{TILE}")
    n = write_tile(args.dst, img)
    print(f"{args.src} -> {args.dst}  ({n} bytes)")
    return 0


def cmd_verify(args) -> int:
    with open(args.src, "rb") as f:
        blob = f.read()
    hdr = parse_header(blob)
    body = len(blob) - HEADER_BYTES
    want = hdr["w"] * hdr["h"] * 2

    print(f"{args.src}")
    print(f"  header    cf={hdr['cf']} w={hdr['w']} h={hdr['h']} "
          f"always_zero={hdr['always_zero']}")
    print(f"  body      {body} bytes, expected {want}")

    ok = True
    if hdr["cf"] != LV_IMG_CF_TRUE_COLOR:
        print(f"  FAIL      cf is {hdr['cf']}, expected {LV_IMG_CF_TRUE_COLOR}")
        ok = False
    if hdr["always_zero"] != 0:
        print("  FAIL      always_zero is not zero; LVGL will reject this")
        ok = False
    if body != want:
        print("  FAIL      body length disagrees with the header")
        ok = False

    if ok and args.dst:
        from_rgb565(blob[HEADER_BYTES:], hdr["w"], hdr["h"]).save(args.dst)
        print(f"  decoded   -> {args.dst}")
    print("  RESULT    " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def synth_tile(z: int, x: int, y: int) -> Image.Image:
    """A tile that says what it is.

    Deliberately not pretty. Each carries its own coordinates, a hard border and
    a centre cross, so the bring-up can tell at a glance whether the right tile
    landed in the right place -- which is the one thing a real map tile would
    make almost impossible to judge.
    """
    # Checkerboard by parity so neighbours never share a shade, and a wrong
    # neighbour shows up as a break in the pattern.
    base = (0x1B, 0x28, 0x33) if (x + y) % 2 == 0 else (0x16, 0x21, 0x2B)
    img = Image.new("RGB", (TILE, TILE), base)
    d = ImageDraw.Draw(img)

    for g in range(0, TILE, 32):
        d.line([(g, 0), (g, TILE)], fill=(0x24, 0x33, 0x40))
        d.line([(0, g), (TILE, g)], fill=(0x24, 0x33, 0x40))

    d.rectangle([0, 0, TILE - 1, TILE - 1], outline=(0x61, 0xDA, 0xFB), width=2)
    c = TILE // 2
    d.line([(c - 14, c), (c + 14, c)], fill=(0xFF, 0xD6, 0x0A), width=3)
    d.line([(c, c - 14), (c, c + 14)], fill=(0xFF, 0xD6, 0x0A), width=3)

    d.text((10, 10), f"z{z}", fill=(0xFF, 0xFF, 0xFF))
    d.text((10, 24), f"x{x}", fill=(0x93, 0xA4, 0xB8))
    d.text((10, 38), f"y{y}", fill=(0x93, 0xA4, 0xB8))
    # Corner brand so a rotated or mirrored blit is obvious.
    d.text((TILE - 30, TILE - 18), "NE" if False else "SE", fill=(0x4F, 0xD3, 0x9A))
    return img


def draw_region(n: int, seed: int) -> Image.Image:
    """Render an n x n tile region as ONE image, to be sliced afterwards.

    Drawn whole rather than per tile because that is the only way roads and
    rivers run continuously across tile seams. Generating each tile
    independently gives nine squares that obviously do not join, which would
    hide exactly the alignment errors these tiles exist to expose.

    Styled dark, like Carto's Dark Matter, because the head unit's UI is dark
    and a white map dropped into it would be blinding at night.
    """
    import random
    rnd = random.Random(seed)

    SS = 2
    span = n * TILE * SS
    img = Image.new("RGB", (span, span), (0x1A, 0x1E, 0x23))
    d = ImageDraw.Draw(img)

    # Parkland first: everything else sits on top of it.
    for _ in range(max(2, n)):
        cx, cy = rnd.randint(0, span), rnd.randint(0, span)
        r = rnd.randint(span // 12, span // 6)
        d.ellipse([cx - r, cy - r * 3 // 4, cx + r, cy + r * 3 // 4],
                  fill=(0x1B, 0x2A, 0x20))

    # A river, wandering top to bottom, drawn under the roads so bridges read
    # as roads crossing water rather than water cutting the road.
    x = rnd.randint(span // 4, span * 3 // 4)
    river = []
    for y in range(-20, span + 20, span // 24):
        x += rnd.randint(-span // 22, span // 22)
        river.append((max(0, min(span, x)), y))
    # Wide and a touch brighter than instinct says: the minor street grid is
    # drawn over it, and at 256px a subtle river simply disappears under the
    # roads.
    d.line(river, fill=(0x1C, 0x3E, 0x5C), width=14 * SS, joint="curve")

    def road(pts, width, colour):
        # Casing under fill: the dark outline is what stops two roads that
        # cross from merging into one blob at this scale.
        d.line(pts, fill=(0x10, 0x14, 0x18), width=width + 3 * SS, joint="curve")
        d.line(pts, fill=colour, width=width, joint="curve")

    # Minor streets: a jittered grid, so blocks look built rather than plotted.
    step = span // (n * 6)
    for i in range(0, span + step, step):
        j = i + rnd.randint(-step // 5, step // 5)
        road([(j, 0), (j, span)], 2 * SS, (0x33, 0x3A, 0x42))
        j = i + rnd.randint(-step // 5, step // 5)
        road([(0, j), (span, j)], 2 * SS, (0x33, 0x3A, 0x42))

    # Secondary roads, a coarser grid on top.
    step2 = span // (n * 2)
    for i in range(step2 // 2, span, step2):
        road([(i, 0), (i, span)], 4 * SS, (0x4E, 0x57, 0x60))
        road([(0, i), (span, i)], 4 * SS, (0x4E, 0x57, 0x60))

    # Two arterials, the brightest thing on the map, deliberately not straight.
    for horizontal in (True, False):
        base = rnd.randint(span // 3, span * 2 // 3)
        pts = []
        for t in range(0, span + 1, span // 10):
            base += rnd.randint(-span // 40, span // 40)
            pts.append((t, base) if horizontal else (base, t))
        road(pts, 7 * SS, (0xC8, 0xA0, 0x50))

    return img.resize((n * TILE, n * TILE), Image.LANCZOS)


def cmd_mapsim(args) -> int:
    region = draw_region(args.n, args.seed)
    total = made = 0
    for dx in range(args.n):
        for dy in range(args.n):
            x, y = args.x + dx, args.y + dy
            tile = region.crop((dx * TILE, dy * TILE, (dx + 1) * TILE, (dy + 1) * TILE))
            path = os.path.join(args.out, str(args.zoom), str(x), f"{y}.bin")
            total += write_tile(path, tile)
            made += 1

    if args.preview:
        region.save(args.preview)
        print(f"preview -> {args.preview}")

    print(f"{made} tiles -> {args.out}/{args.zoom}/  ({total / 1024:.0f} KB)")
    leaf = os.path.basename(os.path.normpath(args.out))
    if leaf != "MAP":
        print(f"WARNING: firmware reads /MAP; this wrote to {leaf!r}")
    print(f"centre tile: /MAP/{args.zoom}/{args.x + args.n // 2}/{args.y + args.n // 2}.bin")
    return 0


def cmd_synth(args) -> int:
    total = 0
    made = 0
    for dx in range(args.n):
        for dy in range(args.n):
            x, y = args.x + dx, args.y + dy
            path = os.path.join(args.out, str(args.zoom), str(x), f"{y}.bin")
            total += write_tile(path, synth_tile(args.zoom, x, y))
            made += 1
    print(f"{made} tiles -> {args.out}/{args.zoom}/")
    print(f"total {total} bytes ({total / 1024:.0f} KB), {total // made} bytes each")
    print()

    # The firmware looks under /MAP, so the output directory has to BE the MAP
    # directory -- naming it anything else produces tiles the device cannot
    # find, which is exactly how the first bring-up went.
    leaf = os.path.basename(os.path.normpath(args.out))
    if leaf != "MAP":
        print(f"WARNING: the firmware reads tiles from /MAP on the card, and this")
        print(f"         wrote to a directory named {leaf!r}. Either rename it to")
        print(f"         MAP or re-run with an output path ending in /MAP.")
        print()
    print("Copy that directory to the SD card root so the card reads:")
    print(f"  /MAP/{args.zoom}/{args.x}/{args.y}.bin")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("synth", help="generate self-labelling test tiles")
    s.add_argument("out")
    s.add_argument("--zoom", type=int, default=15)
    s.add_argument("--x", type=int, default=8721)
    s.add_argument("--y", type=int, default=12556)
    s.add_argument("--n", type=int, default=3, help="n x n grid")
    s.set_defaults(fn=cmd_synth)

    m = sub.add_parser("mapsim", help="fake but map-like tiles, continuous across seams")
    m.add_argument("out")
    m.add_argument("--zoom", type=int, default=15)
    m.add_argument("--x", type=int, default=8721)
    m.add_argument("--y", type=int, default=12556)
    m.add_argument("--n", type=int, default=3, help="n x n grid")
    m.add_argument("--seed", type=int, default=7)
    m.add_argument("--preview", help="also save the whole region as a PNG")
    m.set_defaults(fn=cmd_mapsim)

    c = sub.add_parser("convert", help="image file -> LVGL .bin")
    c.add_argument("src")
    c.add_argument("dst")
    c.set_defaults(fn=cmd_convert)

    v = sub.add_parser("verify", help="check a .bin, optionally decode it back")
    v.add_argument("src")
    v.add_argument("dst", nargs="?")
    v.set_defaults(fn=cmd_verify)

    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
