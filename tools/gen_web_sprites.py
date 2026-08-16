#!/usr/bin/env python3
"""Emit 1:1 RGBA sprite sheets for the website animation.

tools/ascii2png.py renders montages for judging art: scaled up, on an opaque
backdrop, with the eye drawn a loud green so its placement can be checked.
None of that suits compositing over a park in a browser, which needs actual
transparency, true colours, and 1:1 pixels so canvas can upscale by a clean
integer factor.

Every sheet shares one cell geometry -- same width, same height, feet on a
common baseline with `lift` applied -- so the drawing code is a single
blit that never has to know which animation it is showing.

Usage: gen_web_sprites.py <out_dir> [anim ...]
"""
import os
import struct
import sys
import zlib

SRC = os.path.join(os.path.dirname(__file__), 'anim_src')

# The shipped palette, not the art-checking one.
PAL = {
    '.': None,
    '#': (47, 47, 46),
    'w': (224, 224, 224),
    's': (181, 181, 181),
    'p': (222, 117, 134),
    'k': (44, 38, 48),    # eye: dark by day (the firmware greens it at night)
    '^': (44, 38, 48),
    'W': (255, 255, 255),
    'r': (232, 80, 112),
    'z': (150, 170, 196),
}

WANT = ['trot', 'portrait_tail', 'profile_tail', 'sleep', 'clean_paw',
        'pawing', 'leap']


def parse(path):
    frames, cur, lift = [], [], 0
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


def write_rgba(path, w, h, px):
    raw = b''.join(
        b'\x00' + b''.join(bytes(px[y][x]) for x in range(w)) for y in range(h))

    def chunk(tag, d):
        c = tag + d
        return struct.pack('>I', len(d)) + c + struct.pack('>I', zlib.crc32(c))

    open(path, 'wb').write(
        b'\x89PNG\r\n\x1a\n'
        + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
        + chunk(b'IDAT', zlib.compress(raw, 9))
        + chunk(b'IEND', b''))


def main():
    out = sys.argv[1]
    names = sys.argv[2:] or WANT
    os.makedirs(out, exist_ok=True)

    loaded = {n: parse(os.path.join(SRC, f'anim_{n}.txt')) for n in names}

    # One geometry for all of them: tall enough for the biggest pose and the
    # highest lift, wide enough for the sprite.
    sw = len(next(iter(loaded.values()))[0][0][0])
    cell_h = max(len(rows) + lift
                 for fr in loaded.values() for rows, lift in fr)

    manifest = {}
    for name, frames in loaded.items():
        w, h = sw * len(frames), cell_h
        px = [[(0, 0, 0, 0)] * w for _ in range(h)]
        for i, (rows, lift) in enumerate(frames):
            ox = i * sw
            oy = cell_h - len(rows) - lift   # feet on the shared baseline
            for y, row in enumerate(rows):
                for x, ch in enumerate(row):
                    rgb = PAL[ch]
                    if rgb:
                        px[oy + y][ox + x] = (*rgb, 255)
        write_rgba(os.path.join(out, f'cat-{name.replace("_", "-")}.png'),
                   w, h, px)

        # Eyes on their own, in the firmware's night green. At night the
        # engine does not grade the eye colour, it replaces it, so the eyes
        # stay bright while the rest of the cat goes dark. Drawn as a separate
        # layer on top of the tint for exactly that reason.
        eye = [[(0, 0, 0, 0)] * w for _ in range(h)]
        for i, (rows, lift) in enumerate(frames):
            ox = i * sw
            oy = cell_h - len(rows) - lift
            for y, row in enumerate(rows):
                for x, ch in enumerate(row):
                    if ch == 'k':
                        eye[oy + y][ox + x] = (90, 255, 110, 255)
        write_rgba(os.path.join(out, f'cat-eyes-{name.replace("_", "-")}.png'),
                   w, h, eye)
        manifest[name] = len(frames)

    print(f'cell {sw}x{cell_h}')
    for n, c in sorted(manifest.items()):
        print(f'  cat-{n.replace("_", "-")}.png  {c} frames')


if __name__ == '__main__':
    main()
