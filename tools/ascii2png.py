#!/usr/bin/env python3
"""Render ASCII sprite frames to a PNG montage for visual checking.

Input file format: frames separated by lines starting with '---'. A frame may
begin with 'lift=N' (pixels lifted above the floor baseline, for airborne
frames); every other line is a row of the sprite. All rows must be exactly
16 characters.

Charset: . transparent | # outline | w white body | s grey shade | p pink
         k dark eye | ^ happy eye | W pure-white effect | r heart | z zzz

Usage: ascii2png.py frames.txt out.png [scale]
Renders all frames side by side on the checkered backdrop, feet on a common
baseline (minus lift), so a whole animation cycle can be judged at once.
"""
import sys
import struct
import zlib

PAL = {
    '.': None,
    '#': (47, 47, 46),
    'w': (224, 224, 224),
    's': (181, 181, 181),
    'p': (222, 117, 134),
    'k': (80, 255, 120),  # eye marker — rendered loud so placement is checkable
    '^': (44, 38, 48),
    'W': (255, 255, 255),
    'r': (232, 80, 112),
    'z': (150, 170, 196),
}
BG = [(36, 33, 48), (29, 26, 39)]

SPRITE_W = None  # derived from the first frame
CELL_W = 24
CELL_H = 26
BASELINE = 22  # feet row within the cell


def parse(path):
    frames = []
    cur, lift = [], 0
    for line in open(path):
        line = line.rstrip('\n')
        if line.startswith('---'):
            if cur:
                frames.append((cur, lift))
            cur, lift = [], 0
        elif line.startswith('lift='):
            lift = int(line[5:])
        elif line.strip('.') or line == '.' * len(line):
            if line:
                cur.append(line)
    if cur:
        frames.append((cur, lift))
    return frames


def write_png(path, w, h, rgb):
    raw = b''.join(b'\x00' + bytes(v for px in row for v in px) for row in rgb)
    def chunk(tag, data):
        c = tag + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c))
    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw))
           + chunk(b'IEND', b''))
    open(path, 'wb').write(png)


def main():
    frames = parse(sys.argv[1])
    out = sys.argv[2]
    scale = int(sys.argv[3]) if len(sys.argv) > 3 else 6

    global SPRITE_W, CELL_W, CELL_H, BASELINE
    SPRITE_W = len(frames[0][0][0])
    maxrows = max(len(rows) for rows, _ in frames)
    CELL_W = SPRITE_W + 8
    CELL_H = maxrows + 8
    BASELINE = CELL_H - 4
    bad = False
    for i, (rows, _) in enumerate(frames):
        for j, r in enumerate(rows):
            if len(r) != SPRITE_W:
                print(f"frame {i + 1} row {j + 1}: {len(r)} chars (want {SPRITE_W}): {r!r}")
                bad = True
            for ch in r:
                if ch not in PAL:
                    print(f"frame {i + 1} row {j + 1}: bad char {ch!r}")
                    bad = True
    if bad:
        sys.exit(1)

    W, H = CELL_W * len(frames), CELL_H
    img = [[BG[((x // 6) + (y // 6)) % 2] for x in range(W)] for y in range(H)]
    for i, (rows, lift) in enumerate(frames):
        ox = i * CELL_W + (CELL_W - SPRITE_W) // 2
        oy = BASELINE - len(rows) - lift
        for y, row in enumerate(rows):
            for x, ch in enumerate(row):
                c = PAL[ch]
                if c and 0 <= oy + y < H and 0 <= ox + x < W:
                    img[oy + y][ox + x] = c

    big = [[img[y // scale][x // scale] for x in range(W * scale)] for y in range(H * scale)]
    write_png(out, W * scale, H * scale, big)
    print(f"{out}: {len(frames)} frames")


if __name__ == '__main__':
    main()
