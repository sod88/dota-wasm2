#!/usr/bin/env python3
"""Check a frame saved by the native app (cpp/build/lanewars ... --shot frame.bmp): it must be a
picture of the game, not a blank or single-colour window. CI runs this after drawing one frame.

    python3 tools/check_frame.py frame.bmp
"""
import struct, sys

path = sys.argv[1] if len(sys.argv) > 1 else 'frame.bmp'
d = open(path, 'rb').read()
if d[:2] != b'BM' or len(d) < 54:
    sys.exit(f'{path}: not a BMP file')
offset, = struct.unpack_from('<I', d, 10)
w, h = struct.unpack_from('<ii', d, 18)
row = (w * 3 + 3) & ~3
if w < 320 or h < 200 or len(d) < offset + row * h:
    sys.exit(f'{path}: unexpected size {w}x{h}, {len(d)} bytes')

colours, lit, n = set(), 0, 0
for y in range(0, h, 7):
    for x in range(0, w, 7):
        i = offset + y * row + x * 3
        px = d[i:i + 3]
        colours.add(px)
        lit += max(px) > 24
        n += 1
print(f'{path}: {w}x{h}, {len(colours)} colours in {n} samples, {100 * lit // n}% lit')
if len(colours) < 200 or lit < n // 5:
    sys.exit('the frame looks blank: the renderer did not draw the scene')
