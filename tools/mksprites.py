#!/usr/bin/env python3
"""Make PDFS's icon bar sprite and the ResourceFS block the module registers.

    mksprites.py OUT.bin [PREVIEW.png]

The sprite, "pdfs_dev", is a phone drawn as a drive icon: 34x34 pixels at
90 dpi (68x68 OS units, the size of the other filing systems' icons), 32 bits
per pixel with a 1-bit mask. It's drawn at 8x and averaged down; edges are
blended with the icon bar's grey because the mask is only on or off.

The ResourceFS block holds one file, Resources.PDFS.Sprites; the module
registers it and the filer task runs *IconSprites on it.

Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0.
"""
import struct, sys
from PIL import Image, ImageDraw

W = H = 34
S = 8                        # supersampling
BAR = (221, 221, 221)        # icon bar grey, for blending edges


def draw():
    big = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    s = lambda v: int(round(v * S))
    # soft shadow
    d.rounded_rectangle((s(10), s(2.6), s(27.6), s(33.4)), radius=s(4.2), fill=(0, 0, 0, 70))
    # body with a lighter rim
    d.rounded_rectangle((s(8), s(0.6), s(25.6), s(32)), radius=s(4.2), fill=(70, 76, 88, 255))
    d.rounded_rectangle((s(8.8), s(1.4), s(24.8), s(31.2)), radius=s(3.6), fill=(38, 42, 50, 255))
    # screen: blue, lighter at the top
    top, bot = (126, 186, 255), (34, 104, 214)
    x0, y0, x1, y1 = 10.2, 4.0, 23.4, 26.4
    for i in range(s(y0), s(y1)):
        t = (i - s(y0)) / (s(y1) - s(y0))
        c = tuple(int(top[k] + (bot[k] - top[k]) * t) for k in range(3)) + (255,)
        d.line((s(x0), i, s(x1), i), fill=c)
    # a folder on the screen, RISC OS yellow
    fy = 12.6
    d.rounded_rectangle((s(12.4), s(fy - 1.6), s(16.4), s(fy + 0.6)), radius=s(0.6), fill=(214, 160, 30, 255))
    d.rounded_rectangle((s(12.4), s(fy), s(21.2), s(fy + 6.4)), radius=s(0.8), fill=(246, 200, 62, 255))
    d.line((s(12.6), s(fy + 0.5), s(21.0), s(fy + 0.5)), fill=(255, 232, 140, 255), width=s(0.6))
    # earpiece and button
    d.rounded_rectangle((s(14.6), s(2.2), s(19.0), s(2.9)), radius=s(0.35), fill=(90, 96, 108, 255))
    d.ellipse((s(15.4), s(27.6), s(18.2), s(30.4)), outline=(150, 156, 168, 255), width=s(0.55))
    return big.resize((W, H), Image.BOX)


def sprite(img, name):
    rows, mask = [], []
    for y in range(H):
        row, bits = b"", 0
        for x in range(W):
            r, g, b, a = img.getpixel((x, y))
            if a >= 100:
                bits |= 1 << x
                f = a / 255
                r, g, b = (int(r * f + BAR[0] * (1 - f)) if a < 255 else r,
                           int(g * f + BAR[1] * (1 - f)) if a < 255 else g,
                           int(b * f + BAR[2] * (1 - f)) if a < 255 else b)
            else:
                r = g = b = 0
            row += struct.pack("<I", r | g << 8 | b << 16)
        rows.append(row)
        mask.append(struct.pack("<II", bits & 0xFFFFFFFF, bits >> 32))
    image, maskdata = b"".join(rows), b"".join(mask)
    mode = (6 << 27) | (90 << 14) | (90 << 1) | 1       # 32 bpp, 90 x 90 dpi
    hdr_len = 44
    total = hdr_len + len(image) + len(maskdata)
    hdr = struct.pack("<I12sIIIIIII", total, name.encode().ljust(12, b"\0"), W - 1, H - 1, 0, 31,
                      hdr_len, hdr_len + len(image), mode)
    return hdr + image + maskdata


def sprite_file(sprites):
    body = b"".join(sprites)
    # a sprite file is a sprite area without its first word (the area size)
    return struct.pack("<III", len(sprites), 16, 16 + len(body)) + body


def resourcefs(files):
    out = b""
    for name, ftype, data in files:
        n = name.encode() + b"\0"
        n += b"\0" * (-len(n) % 4)
        d = data + b"\0" * (-len(data) % 4)
        load = 0xFFF00000 | ftype << 8
        entry = struct.pack("<IIII", load, 0, len(data), 3) + n + struct.pack("<I", len(data) + 4) + d
        out += struct.pack("<I", 4 + len(entry)) + entry
    return out + struct.pack("<I", 0)


def main():
    img = draw()
    if len(sys.argv) > 2:
        prev = Image.new("RGB", (W, H), BAR)
        prev.paste(img, (0, 0), img)
        prev.resize((W * 8, H * 8), Image.NEAREST).save(sys.argv[2])
    spr = sprite_file([sprite(img, "pdfs_dev")])
    open(sys.argv[1], "wb").write(resourcefs([("Resources.PDFS.Sprites", 0xFF9, spr)]))


if __name__ == "__main__":
    main()
