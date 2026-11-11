#!/usr/bin/env python3
"""Draws the RoCheats Monitor icon (a blue "live status" ring + dot) and writes a
multi-size Windows .ico. Small sizes are drawn glyph-only so they stay legible in
the system tray; larger sizes get a dark rounded tile behind the glyph.

Usage: python3 make_icon.py res/app.ico
"""
import io
import struct
import sys

from PIL import Image, ImageDraw

BLUE = (76, 141, 255, 255)
TILE = (22, 23, 27, 255)
EDGE = (52, 55, 64, 255)
SIZES = [16, 20, 24, 32, 40, 48, 64, 256]


def draw(size: int) -> Image.Image:
    ss = 8  # supersampling factor
    s = size * ss
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c = s / 2
    tile = size >= 32
    if tile:
        m = s * 0.035
        r = s * 0.225
        b = max(ss * 1.0, s * 0.018)
        d.rounded_rectangle([m, m, s - m, s - m], radius=r, fill=EDGE)
        d.rounded_rectangle([m + b, m + b, s - m - b, s - m - b], radius=r - b, fill=TILE)
        ring_r, ring_w, dot_r = s * 0.255, s * 0.066, s * 0.105
    else:
        ring_r, ring_w, dot_r = s * 0.385, s * 0.125, s * 0.17
    outer = ring_r + ring_w / 2
    inner = ring_r - ring_w / 2
    d.ellipse([c - outer, c - outer, c + outer, c + outer], fill=BLUE)
    d.ellipse([c - inner, c - inner, c + inner, c + inner], fill=TILE if tile else (0, 0, 0, 0))
    d.ellipse([c - dot_r, c - dot_r, c + dot_r, c + dot_r], fill=BLUE)
    # Downsample with premultiplied alpha so the edges don't pick up dark fringes.
    return im.convert("RGBa").resize((size, size), Image.LANCZOS).convert("RGBA")


def bmp_entry(im: Image.Image) -> bytes:
    """32-bit BGRA DIB + 1-bit AND mask, as stored inside .ico files."""
    w, h = im.size
    px = im.tobytes("raw", "BGRA")
    rows = [px[y * w * 4:(y + 1) * w * 4] for y in range(h)]
    xor = b"".join(reversed(rows))
    alpha = im.getchannel("A").tobytes()
    stride = ((w + 31) // 32) * 4
    mask = bytearray()
    for y in range(h - 1, -1, -1):
        row = bytearray(stride)
        for x in range(w):
            if alpha[y * w + x] == 0:
                row[x >> 3] |= 0x80 >> (x & 7)
        mask += row
    header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, len(xor) + len(mask), 0, 0, 0, 0)
    return header + xor + bytes(mask)


def png_entry(im: Image.Image) -> bytes:
    buf = io.BytesIO()
    im.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def main() -> None:
    out = sys.argv[1] if len(sys.argv) > 1 else "app.ico"
    blobs = []
    for size in SIZES:
        im = draw(size)
        blobs.append((size, png_entry(im) if size >= 256 else bmp_entry(im)))
    data = struct.pack("<HHH", 0, 1, len(blobs))
    offset = 6 + 16 * len(blobs)
    for size, blob in blobs:
        dim = 0 if size >= 256 else size
        data += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    for _, blob in blobs:
        data += blob
    with open(out, "wb") as f:
        f.write(data)
    if len(sys.argv) > 2:  # optional PNG preview
        draw(256).save(sys.argv[2])
    print(f"wrote {out} ({len(data)} bytes, sizes: {SIZES})")


if __name__ == "__main__":
    main()
