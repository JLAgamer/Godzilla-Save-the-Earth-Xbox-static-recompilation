"""Compare two guest memory dumps (low 64 MB window) and summarise the differences.

usage: memdiff.py A.bin B.bin [--bytes LO HI]
"""
import sys

a = open(sys.argv[1], 'rb').read()
b = open(sys.argv[2], 'rb').read()
P = 4096

if len(sys.argv) > 3 and sys.argv[3] == '--bytes':
    lo, hi = int(sys.argv[4], 16), int(sys.argv[5], 16)
    for off in range(lo, hi, 4):
        x, y = a[off:off + 4], b[off:off + 4]
        if x != y:
            print('%08X: %s  %s' % (off, x[::-1].hex(), y[::-1].hex()))
    sys.exit()

pages = []
for p in range(len(a) // P):
    o = p * P
    if a[o:o + P] != b[o:o + P]:
        n = sum(1 for i in range(o, o + P) if a[i] != b[i])
        pages.append((p, n))

# group consecutive pages into ranges
ranges = []
for p, n in pages:
    if ranges and ranges[-1][1] == p - 1:
        ranges[-1][1] = p
        ranges[-1][2] += n
    else:
        ranges.append([p, p, n])

print('%d differing pages, %d ranges' % (len(pages), len(ranges)))
for lo, hi, n in ranges[:80]:
    print('  %08X-%08X  %5d pages  %8d bytes' % (lo * P, (hi + 1) * P, hi - lo + 1, n))
