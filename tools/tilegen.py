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
import math
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


# Map style, shared by both renderers so raster and vector cannot drift apart.
STYLE = {
    "ground": (0x1A, 0x1E, 0x23),
    "park":   (0x1B, 0x2A, 0x20),
    "water":  (0x1C, 0x3E, 0x5C),
    "casing": (0x10, 0x14, 0x18),
    "minor":  (0x33, 0x3A, 0x42),
    "second": (0x4E, 0x57, 0x60),
    "artery": (0xC8, 0xA0, 0x50),
}


def region_geometry(n: int, seed: int):
    """The region as primitives in normalised 0..1 coordinates.

    Normalised on purpose: the same geometry then renders at any pixel size,
    which is what lets the raster tiles and the vector drawing below be the
    same map rather than two similar-looking ones. It is also the shape the
    data would take on the device -- coordinates, not pixels.
    """
    import random
    rnd = random.Random(seed)
    parks, water, roads = [], [], []

    for _ in range(max(2, n)):
        cx, cy = rnd.random(), rnd.random()
        r = rnd.uniform(1 / 12, 1 / 6)
        parks.append((cx, cy, r, r * 0.75))

    x = rnd.uniform(0.25, 0.75)
    river = []
    for i in range(26):
        x += rnd.uniform(-1 / 22, 1 / 22)
        river.append((min(1.0, max(0.0, x)), -0.03 + i / 24))
    water.append(river)

    step = 1 / (n * 6)
    i = 0.0
    while i <= 1.0 + step:
        j = i + rnd.uniform(-step / 5, step / 5)
        roads.append(("minor", [(j, -0.02), (j, 1.02)]))
        j = i + rnd.uniform(-step / 5, step / 5)
        roads.append(("minor", [(-0.02, j), (1.02, j)]))
        i += step

    step2 = 1 / (n * 2)
    i = step2 / 2
    while i < 1.0:
        roads.append(("second", [(i, -0.02), (i, 1.02)]))
        roads.append(("second", [(-0.02, i), (1.02, i)]))
        i += step2

    for horizontal in (True, False):
        base = rnd.uniform(1 / 3, 2 / 3)
        pts = []
        for t in range(11):
            base += rnd.uniform(-1 / 40, 1 / 40)
            u = t / 10
            pts.append((u, base) if horizontal else (base, u))
        roads.append(("artery", pts))

    return parks, water, roads


# Stroke widths as a fraction of one tile, so they hold at any render scale.
ROAD_W = {"minor": 2 / 256, "second": 4 / 256, "artery": 7 / 256}
WATER_W = 14 / 256
CASING_W = 3 / 256


def render_raster(n, seed, px, ss=2):
    """Rasterise the region at `px` pixels square."""
    parks, water, roads = region_geometry(n, seed)
    span = px * ss
    img = Image.new("RGB", (span, span), STYLE["ground"])
    d = ImageDraw.Draw(img)
    S = lambda p: (p[0] * span, p[1] * span)
    w = lambda frac: max(1, int(frac * span / n))

    for cx, cy, rx, ry in parks:
        d.ellipse([(cx - rx) * span, (cy - ry) * span,
                   (cx + rx) * span, (cy + ry) * span], fill=STYLE["park"])
    for line in water:
        d.line([S(p) for p in line], fill=STYLE["water"], width=w(WATER_W), joint="curve")
    for kind, pts in roads:
        xy = [S(p) for p in pts]
        d.line(xy, fill=STYLE["casing"], width=w(ROAD_W[kind]) + w(CASING_W), joint="curve")
        d.line(xy, fill=STYLE[kind], width=w(ROAD_W[kind]), joint="curve")
    return img.resize((px, px), Image.LANCZOS)


def render_svg(n, seed, px):
    """The same region as SVG -- geometry, not pixels."""
    parks, water, roads = region_geometry(n, seed)
    hexc = lambda c: "#%02x%02x%02x" % c
    w = lambda frac: frac * px / n
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{px}" height="{px}" '
           f'viewBox="0 0 {px} {px}">',
           f'<rect width="{px}" height="{px}" fill="{hexc(STYLE["ground"])}"/>']
    for cx, cy, rx, ry in parks:
        out.append(f'<ellipse cx="{cx*px:.1f}" cy="{cy*px:.1f}" rx="{rx*px:.1f}" '
                   f'ry="{ry*px:.1f}" fill="{hexc(STYLE["park"])}"/>')
    pts_s = lambda pts: " ".join(f"{x*px:.1f},{y*px:.1f}" for x, y in pts)
    for line in water:
        out.append(f'<polyline points="{pts_s(line)}" fill="none" '
                   f'stroke="{hexc(STYLE["water"])}" stroke-width="{w(WATER_W):.1f}" '
                   f'stroke-linejoin="round" stroke-linecap="round"/>')
    for kind, pts in roads:
        out.append(f'<polyline points="{pts_s(pts)}" fill="none" '
                   f'stroke="{hexc(STYLE["casing"])}" '
                   f'stroke-width="{w(ROAD_W[kind])+w(CASING_W):.1f}" '
                   f'stroke-linejoin="round" stroke-linecap="round"/>')
    for kind, pts in roads:
        out.append(f'<polyline points="{pts_s(pts)}" fill="none" stroke="{hexc(STYLE[kind])}" '
                   f'stroke-width="{w(ROAD_W[kind]):.1f}" '
                   f'stroke-linejoin="round" stroke-linecap="round"/>')
    out.append("</svg>")
    return "\n".join(out)


def draw_region(n: int, seed: int) -> Image.Image:
    return render_raster(n, seed, n * TILE)


# Must match RoadMap.h.
ROAD_CLASS = {"minor": 0, "second": 1, "artery": 2, "water": 3}


def cmd_roads(args) -> int:
    """The same region as geometry, in the format the firmware reads.

    Deliberately the SAME region_geometry() the tiles come from, so the two
    can be compared like for like rather than as two maps that merely look
    similar.
    """
    parks, water, roads = region_geometry(args.n, args.seed)

    # Normalised 0..1 onto a real patch of the world, so the projection and
    # the fit-to-bounds code get plausible degrees rather than unit squares.
    lat0, lon0 = args.lat, args.lon
    span_deg = args.span_km / 111.32

    def enc(u, v):
        # u across, v down. v is subtracted because latitude grows north while
        # the rendered image grows downward -- the same flip Map_Project makes.
        lat = lat0 + (0.5 - v) * span_deg
        lon = lon0 + (u - 0.5) * span_deg / math.cos(math.radians(lat0))
        return int(round(lat * 1e7)), int(round(lon * 1e7))

    ways = []
    for line in water:
        ways.append((ROAD_CLASS["water"], [enc(u, v) for u, v in line]))
    for kind, pts in roads:
        ways.append((ROAD_CLASS[kind], [enc(u, v) for u, v in pts]))

    lats = [p[0] for _, pts in ways for p in pts]
    lons = [p[1] for _, pts in ways for p in pts]

    blob = bytearray()
    blob += b"PRD1"
    blob += struct.pack("<I", len(ways))
    blob += struct.pack("<iiii", min(lats), min(lons), max(lats), max(lons))
    for klass, pts in ways:
        blob += struct.pack("<BBH", klass, 0, len(pts))
        for la, lo in pts:
            blob += struct.pack("<ii", la, lo)

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(blob)

    points = sum(len(p) for _, p in ways)
    tiles = args.n * args.n * (TILE * TILE * 2 + 4)
    print(f"{len(ways)} ways, {points} points -> {args.out}")
    print(f"  {len(blob)} bytes ({len(blob)/1024:.1f} KB)")
    print(f"  same area as raster tiles: {tiles} bytes ({tiles/1024:.0f} KB)")
    print(f"  {tiles/len(blob):.0f}x smaller")
    return 0


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

    r = sub.add_parser("roads", help="the same region as vector geometry (.prd)")
    r.add_argument("out")
    r.add_argument("--n", type=int, default=3)
    r.add_argument("--seed", type=int, default=7)
    r.add_argument("--lat", type=float, default=40.0992)
    r.add_argument("--lon", type=float, default=-83.1141)
    r.add_argument("--span-km", type=float, default=2.8,
                   help="ground width of the whole region")
    r.set_defaults(fn=cmd_roads)

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
