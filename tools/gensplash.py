#!/usr/bin/env python3
"""Turn an image into the boot splash the panel shows while it starts up.

LVGL can decode a PNG at runtime, and doing that here would be the wrong trade:
the splash has to appear before anything else, decoding costs a 150KB buffer
and a second of CPU at the exact moment the board has neither to spare, and a
decoder failure would leave a blank screen with nothing to report it. So the
image is converted here, once, into the pixels the panel wants -- the firmware
does nothing at boot but hand a pointer to the display driver.

Output is LV_IMG_CF_TRUE_COLOR: an array of lv_color_t, which at
LV_COLOR_DEPTH 16 is RGB565, little-endian, one uint16 per pixel. That matches
LV_COLOR_16_SWAP 0 in include/lv_conf.h. If the splash ever comes out with red
and blue exchanged, those two settings have drifted apart and this is the file
that has to follow.

The image is scaled to COVER 240x320 and centre-cropped, never letterboxed:
bars down the sides of a boot screen read as a broken image rather than as a
deliberate frame.

`--inset N` trims N pixels off every edge of the SOURCE first. Artwork of a
device tends to be drawn with its bezel, and a bezel rendered inside a real
bezel reads as a photograph of the thing rather than as the thing. Trimming
before the scale rather than after is what makes the illustration larger
instead of merely cropped.

`--shift N` slides the finished picture N pixels DOWN the frame. A title drawn
hard against the top of its artwork sits hard against the bezel once it is on
a panel, which reads as a crop rather than as a margin. The cover crop has a
little vertical slack to spend first; past that, the top row is repeated to
fill. That is only honest while the top row is flat -- sky, in this artwork --
and would be visible as a smear on anything with detail up there.

`--indexed` writes LV_IMG_CF_INDEXED_8BIT instead of true colour: a 256-entry
palette followed by one byte per pixel, which LVGL looks up as it draws. It
halves the array with no decoder and no buffer, so it keeps everything the
paragraph above is about. The palette costs 1KB and the artwork has to survive
256 colours -- measure before assuming it does.

    python tools/gensplash.py <in.png|jpg> <out.c> [--inset N] [--shift N] [--indexed]
"""
import sys

from PIL import Image

W = 240
H = 320
NAME = "pegasus_splash"


def cover(img, shift=0):
    """Scale to fill W x H and crop the overflow off the centre, then slide the
    result `shift` pixels down the frame.

    The slide spends the crop's own vertical slack first -- moving the window
    up the scaled image shows more of its top and costs nothing invented. Only
    once that runs out is the top row repeated to fill, and the caller is
    expected to have checked that the top row is flat enough to repeat.
    """
    scale = max(W / img.width, H / img.height)
    resized = img.resize((max(W, round(img.width * scale)), max(H, round(img.height * scale))),
                         Image.LANCZOS)
    left = (resized.width - W) // 2
    column = resized.crop((left, 0, left + W, resized.height))

    top = (column.height - H) // 2 - shift
    pad = max(0, -top)
    top = max(0, top)

    out = Image.new("RGB", (W, H))
    if pad:
        out.paste(column.crop((0, 0, W, 1)).resize((W, pad), Image.NEAREST), (0, 0))
    out.paste(column.crop((0, top, W, min(column.height, top + H - pad))), (0, pad))
    return out


def main():
    args = sys.argv[1:]
    inset = 0
    shift = 0
    indexed = False
    if "--inset" in args:
        i = args.index("--inset")
        inset = int(args[i + 1])
        del args[i:i + 2]
    if "--shift" in args:
        i = args.index("--shift")
        shift = int(args[i + 1])
        del args[i:i + 2]
    if "--indexed" in args:
        indexed = True
        args.remove("--indexed")
    if len(args) != 2:
        print(__doc__)
        return 1

    src = Image.open(args[0]).convert("RGB")
    if inset > 0:
        if 2 * inset >= min(src.width, src.height):
            print(f"--inset {inset} leaves nothing of a {src.width}x{src.height} image")
            return 1
        src = src.crop((inset, inset, src.width - inset, src.height - inset))
    img = cover(src, shift)

    blob = bytearray()
    if indexed:
        # Median cut, undithered. Dithering a sky is the one thing to avoid
        # here: it trades banding nobody sees in two seconds for a stipple that
        # looks like screen noise, and it also destroys the run-of-identical-
        # bytes structure that makes this array compress on the host.
        pal = img.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
        table = pal.getpalette()[:256 * 3]
        table += [0] * (256 * 3 - len(table))  # a flat image may use fewer than 256

        # lv_color32_t is {blue, green, red, alpha} byte for byte, and LVGL
        # reads exactly 256 of them before the first index whether or not the
        # image uses them all. Alpha is 0xFF throughout: the splash is opaque
        # and LVGL still consults the byte.
        for i in range(256):
            r, g, b = table[3 * i:3 * i + 3]
            blob += bytes((b, g, r, 0xFF))
        blob += pal.tobytes()
    else:
        for y in range(H):
            for x in range(W):
                r, g, b = img.getpixel((x, y))
                # 8 bits down to 5/6/5. Truncation rather than dithering: the
                # banding it avoids is invisible for two seconds, and dithering
                # a flat sky produces a texture that looks like screen noise.
                pixel = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
                blob.append(pixel & 0xFF)   # little-endian, as lv_color_t is stored
                blob.append(pixel >> 8)

    with open(args[1], "w") as f:
        f.write("// GENERATED by tools/gensplash.py -- do not edit by hand.\n")
        f.write(f"// Regenerate: python tools/gensplash.py <image> {args[1]}"
                f"{f' --inset {inset}' if inset else ''}"
                f"{f' --shift {shift}' if shift else ''}"
                f"{' --indexed' if indexed else ''}\n")
        f.write("//\n")
        if indexed:
            f.write(f"// {W}x{H} INDEXED_8BIT: 256 lv_color32_t of palette, then one byte\n")
            f.write("// per pixel. Half the size of true colour and still no decoder and no\n")
            f.write("// buffer at boot -- see the tool for why that matters at this moment.\n\n")
        else:
            f.write(f"// {W}x{H} RGB565, the panel's own format, so the boot path does no\n")
            f.write("// decoding at all -- see the tool for why that matters at this moment.\n\n")
        f.write('#include "lvgl.h"\n\n')
        f.write("static const uint8_t splash_map[] = {\n")
        for i in range(0, len(blob), 16):
            f.write("    " + ", ".join(f"0x{b:02x}" for b in blob[i:i + 16]) + ",\n")
        f.write("};\n\n")
        f.write(f"const lv_img_dsc_t {NAME} = {{\n")
        cf = "LV_IMG_CF_INDEXED_8BIT" if indexed else "LV_IMG_CF_TRUE_COLOR"
        f.write(f"    .header = {{.cf = {cf}, .always_zero = 0, .reserved = 0,\n")
        f.write(f"               .w = {W}, .h = {H}}},\n")
        f.write("    .data_size = sizeof(splash_map),\n")
        f.write("    .data = splash_map,\n")
        f.write("};\n")

    print(f"{NAME}: {W}x{H} {'indexed' if indexed else 'true colour'}"
          f"{f', shifted down {shift}px' if shift else ''}, {len(blob) / 1024:.1f} KB of flash")
    return 0


if __name__ == "__main__":
    sys.exit(main())
