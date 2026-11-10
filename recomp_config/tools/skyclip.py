"""Simulate the CPU path's clip (w > 1e-3, raster_xf_clipped) for the sky faces
logged in a trace, and report which faces cover given screen pixels."""
import re
import sys

faces = []
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    m = re.search(r'\| all (.*?) idx ([0-9 ]+)', line)
    if not m:
        continue
    verts = [tuple(map(float, v.split())) for v in re.findall(r'\[([^\]]+)\]', m.group(1))]
    idx = list(map(int, m.group(2).split()))
    faces.append((verts, idx))
    if len(faces) == 5:
        break

W_EPS = 1e-3


def clip_w(poly):
    out = []
    for i in range(len(poly)):
        a, b = poly[i], poly[(i + 1) % len(poly)]
        da, db = a[3] - W_EPS, b[3] - W_EPS
        if da >= 0:
            out.append(a)
        if (da >= 0) != (db >= 0):
            t = da / (da - db)
            out.append(tuple(a[k] + (b[k] - a[k]) * t for k in range(4)))
    return out


def covers(tri, px, py):
    (x0, y0), (x1, y1), (x2, y2) = tri
    d = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0)
    if d == 0:
        return False
    a = ((x1 - px) * (y2 - py) - (y1 - py) * (x2 - px)) / d
    b = ((x2 - px) * (y0 - py) - (y2 - py) * (x0 - px)) / d
    c = 1 - a - b
    return a >= 0 and b >= 0 and c >= 0


pts = [(600, 40), (600, 120), (600, 200), (520, 40), (300, 100)]
for fi, (verts, idx) in enumerate(faces):
    # homogeneous: screen x, y were divided by w; clip space is (x*w, y*w, z*w, w)
    hom = [(x * w, y * w, z * w, w) for x, y, z, w in verts]
    hit = {p: False for p in pts}
    for t in range(0, len(idx) - 2, 3):
        poly = clip_w([hom[idx[t]], hom[idx[t + 1]], hom[idx[t + 2]]])
        scr = [(v[0] / v[3], v[1] / v[3]) for v in poly]
        for k in range(1, len(scr) - 1):
            for p in pts:
                if covers((scr[0], scr[k], scr[k + 1]), *p):
                    hit[p] = True
    ws = ' '.join('%+.0f' % v[3] for v in verts)
    print('face d%d w [%s]: ' % (fi + 1, ws) + '  '.join('%s:%s' % (p, 'Y' if hit[p] else '.') for p in pts))
