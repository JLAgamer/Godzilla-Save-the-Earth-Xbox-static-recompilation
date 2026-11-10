"""Find strings in the XBE and print their virtual addresses plus any code
references in the generated C.  Usage: xbestr.py <xbe> <gen dir> <text>..."""
import sys, struct, glob, re
d = open(sys.argv[1], 'rb').read(); gen = sys.argv[2]
base, = struct.unpack_from('<I', d, 0x104); nsec, sech = struct.unpack_from('<II', d, 0x11C)
secs = []
for i in range(nsec):
    fl, va, vs, ra, rs = struct.unpack_from('<5I', d, sech - base + i * 56)
    secs.append((va, vs, ra, rs))
def va_of(off):
    for va, vs, ra, rs in secs:
        if ra <= off < ra + rs: return va + off - ra
src = None
for t in sys.argv[3:]:
    tb = t.encode()
    for m in re.finditer(re.escape(tb), d):
        s = m.start()
        while s > 0 and 32 <= d[s - 1] < 127: s -= 1
        e = d.index(b'\0', m.start()); va = va_of(s)
        if va is None: continue
        if src is None: src = {f: open(f, errors='replace').read() for f in glob.glob(gen + '/*.c')}
        refs = []
        for f, txt in src.items():
            for r in re.finditer(r'0x0*%X\b' % va, txt):
                fn = re.findall(r'\nvoid (sub_[0-9A-F]+)', txt[:r.start()])
                refs.append(fn[-1] if fn else '?')
        print('%08X %-40s refs: %s' % (va, d[s:e].decode(errors='replace')[:40], ' '.join(sorted(set(refs))) or '-'))
