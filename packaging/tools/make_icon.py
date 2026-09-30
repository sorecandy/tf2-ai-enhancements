r"""make_icon.py - writes the package icon (256x256 PNG, original artwork drawn in code) to package_files/icon.png.

Deterministic: same pixels every run (no fonts, no randomness, no PIL): a tiny rasteriser + a zlib PNG writer.

usage: python tools\make_icon.py [out.png]
"""
import math
import os
import struct
import sys
import zlib

W = H = 256


def render():
    """Returns rows of RGB bytes: a dark slate tile, a cyan 'lens' ring with a 3x3 pixel grid (upscaling) and two
    offset chevrons (generated frames)."""
    px = bytearray(W * H * 3)

    def put(x, y, c, a=1.0):
        if 0 <= x < W and 0 <= y < H:
            i = (y * W + x) * 3
            for k in range(3):
                px[i + k] = int(round(px[i + k] * (1 - a) + c[k] * a))

    # background: vertical gradient slate -> near black, rounded corners
    for y in range(H):
        t = y / (H - 1)
        base = (int(28 - 14 * t), int(34 - 16 * t), int(48 - 22 * t))
        for x in range(W):
            r = 36
            dx = max(r - x, x - (W - 1 - r), 0)
            dy = max(r - y, y - (H - 1 - r), 0)
            d = math.hypot(dx, dy)
            a = 1.0 if d <= r - 1 else (0.0 if d >= r else r - d)
            i = (y * W + x) * 3
            px[i:i + 3] = bytes((int(base[0] * a), int(base[1] * a), int(base[2] * a)))

    cyan = (64, 220, 255)
    green = (120, 255, 150)
    cx, cy = 112, 128
    # ring (anti-aliased by distance)
    for y in range(H):
        for x in range(W):
            d = math.hypot(x + 0.5 - cx, y + 0.5 - cy)
            e = abs(d - 72)
            if e < 9:
                put(x, y, cyan, min(1.0, 9 - e) if e > 8 else 1.0)
    # 3x3 grid inside the ring: small squares, brightest in the centre
    for gy in range(3):
        for gx in range(3):
            x0 = cx - 42 + gx * 30
            y0 = cy - 42 + gy * 30
            lit = 1.0 if (gx, gy) == (1, 1) else 0.55
            col = tuple(int(c * lit) for c in cyan)
            for y in range(y0, y0 + 22):
                for x in range(x0, x0 + 22):
                    put(x, y, col)
    # two chevrons to the right (frame generation), green then cyan
    for k, col in ((0, green), (1, cyan)):
        ox = 222 + k * 26
        for y in range(H):
            for x in range(W):
                dy = y + 0.5 - cy
                if abs(dy) > 44:
                    continue
                edge = ox - abs(dy) * 0.68 - 13
                e = (x + 0.5) - edge
                if 0 <= e <= 13:
                    put(x, y, col)
    return px


def write_png(path, px):
    raw = b''.join(b'\x00' + bytes(px[y * W * 3:(y + 1) * W * 3]) for y in range(H))

    def chunk(t, data):
        return struct.pack('>I', len(data)) + t + data + struct.pack('>I', zlib.crc32(t + data) & 0xffffffff)

    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) + \
        chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
    tmp = path + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(png)
    os.replace(tmp, path)


if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                                             'package_files', 'icon.png')
    write_png(out, render())
    print('wrote', out)
