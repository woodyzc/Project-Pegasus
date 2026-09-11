#!/usr/bin/env python3
"""Map tiles for the head unit: convert, verify, and synthesise.

Step 1 of the offline-map scope. The device cannot decode PNG -- there is no
decoder in the firmware and no room to want one -- so tiles are converted to
LVGL's own raw format on a computer and the firmware just reads bytes.

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
    print("Copy the MAP directory to the SD card root, then the firmware path is:")
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
