"""Contact sheet of dumped textures (GZ_TEX_DUMP) no larger than a limit, on a
grey checker so alpha shows.  Usage: texsheet.py <dump dir> <out.png> [max side=64] [cell=72]"""
import sys, os, struct, zlib
d, out = sys.argv[1], sys.argv[2]; lim = int(sys.argv[3]) if len(sys.argv) > 3 else 64
cell = int(sys.argv[4]) if len(sys.argv) > 4 else 72
items = []
for f in sorted(os.listdir(d)):
    if not f.endswith('.bmp'): continue
    b = open(os.path.join(d, f), 'rb').read(); w, h = struct.unpack_from('<ii', b, 18); off, = struct.unpack_from('<I', b, 10)
    if max(w, h) > lim: continue
    items.append((f, w, h, b[off:off + w * h * 4]))
cols = 10; rows = (len(items) + cols - 1) // cols; W, H = cols * cell, rows * cell
img = bytearray(W * H * 3)
for i in range(W * H):
    x, y = i % W, i // W; v = 90 if ((x // 8) + (y // 8)) & 1 else 60
    img[i * 3:i * 3 + 3] = bytes((v, v, v))
for n, (f, w, h, px) in enumerate(items):
    ox, oy = (n % cols) * cell + 2, (n // cols) * cell + 2
    for y in range(min(h, cell - 4)):
        for x in range(min(w, cell - 4)):
            o = ((h - 1 - y) * w + x) * 4; b_, g, r, a = px[o], px[o + 1], px[o + 2], px[o + 3]
            i = ((oy + y) * W + ox + x) * 3
            img[i] = (r * a + img[i] * (255 - a)) // 255; img[i + 1] = (g * a + img[i + 1] * (255 - a)) // 255; img[i + 2] = (b_ * a + img[i + 2] * (255 - a)) // 255
    print(n, 'row', n // cols, 'col', n % cols, f)
raw = b''.join(b'\0' + bytes(img[y * W * 3:(y + 1) * W * 3]) for y in range(H))
def ch(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))
open(out, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) + ch(b'IDAT', zlib.compress(raw)) + ch(b'IEND', b''))
