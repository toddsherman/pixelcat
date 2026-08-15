#!/usr/bin/env python3
"""Generate main/cat_bg.c from background frame PNGs (RGBA or indexed).

Area-averages each frame down to a coarse pixel grid (panel / block), then
rebuilds it as hard-edged blocks at panel resolution, so the scene reads as
pixel art next to the sprite. block=1 is a smooth full-res backdrop.

Every frame is baked in five time-of-day grades (day, dawn, dusk, twilight,
night), emitted as cat_bg[variant][frame] with a matching enum in cat_bg.h.

Usage: gen_bg.py out_dir block frame1.png [frame2.png ...]
"""
import sys, struct, zlib

PANEL_W, PANEL_H = 368, 448   # physical panel (portrait)
VIEW_W, VIEW_H = 448, 368     # logical landscape the viewer sees
# Landscape crop window of the portrait source, chosen to keep the pond and
# path: full width, a 410-row band starting below the canopy.
CROP_Y0_FRAC, CROP_H_FRAC = 0.22, 0.685

def decode_png(path):
    d = open(path, 'rb').read()
    pos, idat, w, h, ctype, plte = 8, b'', 0, 0, 6, b''
    while pos < len(d):
        ln = int.from_bytes(d[pos:pos+4], 'big'); tag = d[pos+4:pos+8]
        if tag == b'IHDR':
            w, h = struct.unpack('>II', d[pos+8:pos+16])
            ctype = d[pos+17]
            assert d[pos+16] == 8 and ctype in (3, 6), f'unsupported PNG (ctype {ctype})'
        elif tag == b'PLTE':
            plte = d[pos+8:pos+8+ln]
        elif tag == b'IDAT':
            idat += d[pos+8:pos+8+ln]
        pos += 12 + ln
    raw = zlib.decompress(idat)
    bpp = 4 if ctype == 6 else 1
    stride = w * bpp
    img = bytearray(w * h * 4)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p+stride]); p += stride
        if f == 1:
            for i in range(bpp, stride): line[i] = (line[i] + line[i-bpp]) & 255
        elif f == 2:
            for i in range(stride): line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                a = line[i-bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i-bpp] if i >= bpp else 0
                b = prev[i]; c = prev[i-bpp] if i >= bpp else 0
                pa, pb, pc = abs(b-c), abs(a-c), abs(a+b-2*c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        img[y*stride:(y+1)*stride] = line
        prev = line
    if ctype == 3:
        rgba = bytearray(w * h * 4)
        for i in range(w * h):
            pi = img[i] * 3
            rgba[i*4:i*4+4] = bytes((plte[pi], plte[pi+1], plte[pi+2], 255))
        return w, h, rgba
    return w, h, img

def resize_565(w, h, img, block):
    # Crop the portrait source to a landscape window, sample to the logical
    # landscape grid, then rotate into the portrait panel layout so the
    # firmware can keep block-copying rows.
    cy0, ch = int(h * CROP_Y0_FRAC), int(h * CROP_H_FRAC)
    gw, gh = VIEW_W // block, VIEW_H // block
    cells = []
    for oy in range(gh):
        sy0, sy1 = cy0 + oy * ch / gh, cy0 + (oy + 1) * ch / gh
        for ox in range(gw):
            sx0, sx1 = ox * w / gw, (ox + 1) * w / gw
            r = g = b = n = 0
            for yy in range(int(sy0), max(int(sy0)+1, int(sy1))):
                for xx in range(int(sx0), max(int(sx0)+1, int(sx1))):
                    i = (min(yy, h-1) * w + min(xx, w-1)) * 4
                    r += img[i]; g += img[i+1]; b += img[i+2]; n += 1
            r //= n; g //= n; b //= n
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            cells.append(((v >> 8) & 0xFF) | ((v & 0xFF) << 8))  # byte-swapped
    # Expand to logical landscape resolution as hard blocks, then rotate
    # 90 degrees into the portrait panel layout: panel(px, py) shows
    # logical(lx = py, ly = VIEW_H - 1 - px).
    view = []
    for oy in range(VIEW_H):
        crow = cells[min(oy // block, gh - 1) * gw:]
        for ox in range(VIEW_W):
            view.append(crow[min(ox // block, gw - 1)])
    out = []
    for py in range(PANEL_H):
        for px in range(PANEL_W):
            out.append(view[(VIEW_H - 1 - px) * VIEW_W + py])
    return out

def mixg(c, gray, k):
    return c + (gray - c) * k

# Ordered: the firmware's daypart enum must match.
GRADES = [
    ('DAY',      lambda r, g, b, gy: (r, g, b)),
    ('DAWN',     lambda r, g, b, gy: (r*.82+34, g*.68+16, b*.78+26)),
    ('DUSK',     lambda r, g, b, gy: (r*.92+28, g*.62+8, b*.45+6)),
    ('TWILIGHT', lambda r, g, b, gy: (mixg(r,gy,.3)*.48+22, mixg(g,gy,.3)*.38+10, mixg(b,gy,.3)*.62+34)),
    ('NIGHT',    lambda r, g, b, gy: (mixg(r,gy,.45)*.24+6, mixg(g,gy,.45)*.28+9, mixg(b,gy,.45)*.52+28)),
]

def grade(img, fn):
    out = bytearray(img)
    for i in range(0, len(out), 4):
        r, g, b = out[i], out[i+1], out[i+2]
        gy = (r * 30 + g * 59 + b * 11) // 100
        r2, g2, b2 = fn(r, g, b, gy)
        out[i] = max(0, min(255, int(r2)))
        out[i+1] = max(0, min(255, int(g2)))
        out[i+2] = max(0, min(255, int(b2)))
    return out

def main():
    out_dir = sys.argv[1]
    block = int(sys.argv[2])
    sources = [decode_png(p) for p in sys.argv[3:]]
    n = len(sources)
    nv = len(GRADES)
    with open(out_dir + '/cat_bg.c', 'w') as f:
        f.write('// GENERATED by tools/gen_bg.py — do not hand-edit.\n')
        f.write('#include "cat_bg.h"\n\n')
        f.write(f'const uint16_t cat_bg[{nv}][{n}][{PANEL_W*PANEL_H}] = {{\n')
        for vname, fn in GRADES:
            f.write('{\n')
            for (w, h, img) in sources:
                fr = resize_565(w, h, grade(img, fn), block)
                f.write('{\n')
                for i in range(0, len(fr), 16):
                    f.write(','.join(f'0x{v:04x}' for v in fr[i:i+16]) + ',\n')
                f.write('},\n')
            f.write('},\n')
            print(f'{vname} baked')
        f.write('};\n')
    with open(out_dir + '/cat_bg.h', 'w') as f:
        f.write('#pragma once\n#include <stdint.h>\n\n')
        f.write(f'#define BG_FRAMES {n}\n')
        f.write(f'#define BG_VARIANTS {nv}\n')
        f.write(f'#define BG_W {PANEL_W}\n#define BG_H {PANEL_H}\n\n')
        for vi, (vname, _) in enumerate(GRADES):
            f.write(f'#define BG_{vname} {vi}\n')
        f.write(f'\nextern const uint16_t cat_bg[{nv}][{n}][{PANEL_W*PANEL_H}];\n')
    print(f'{nv} variants x {n} frames -> cat_bg.c')

main()
