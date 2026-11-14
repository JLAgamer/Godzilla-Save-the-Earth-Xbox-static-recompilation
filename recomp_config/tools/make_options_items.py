"""Add five entries to the Options menu: tournament mode, button icons,
frame rate, aspect ratio and game speed.

The Options screen's layout is XML text inside ShellData/ControlRoom.bdl
(<interface n="Options">); its MainBox list holds AudioAdjust,
ControllerAdjust and TopTen. This writes a copy of the bundle with five more
list entries, GzTournament, GzPrompts, GzRate, GzWide and GzSpeed, and moves the entries
closer together so they stay above the Audio / Controls panels. The game's own Options script does
nothing for names it does not know; the recomp reacts to them (see "Options
menu toggles" in src/recomp_manual.c) and sets their text.

The bundle is a packed binary, so the XML keeps its exact length: the new
entries are paid for with indentation tabs from the same interface.

Usage: make_options_items.py <original ControlRoom.bdl> <output ControlRoom.bdl>
"""
import re
import sys

src, dst = sys.argv[1], sys.argv[2]
b = open(src, 'rb').read()
i = b.find(b'<interface n="Options">')
j = b.find(b'</interface>', i)
assert i >= 0 and j > i, 'Options interface not found'
x = b[i:j]
orig_len = len(x)
assert b'GzTournament' not in x, 'already patched'

# The TopTen entry is the template for the new ones.
m = re.search(rb'<text n="TopTen">.*?</text>', x, re.S)
assert m
tpl = re.sub(rb'[\t\r\n]+', b'', m.group(0))
ys = [float(v) for v in re.findall(rb'<text n="(?:AudioAdjust|ControllerAdjust|TopTen)">\s*<position> 0 ([0-9.]+) 0', x)]
assert len(ys) == 3, ys
STEP = 24.0                     # original spacing is 34.85; eight entries end where seven at 28 did
new = b''
for k, name in enumerate((b'GzTournament', b'GzPrompts', b'GzRate', b'GzWide', b'GzSpeed')):
    t = tpl.replace(b'n="TopTen"', b'n="' + name + b'"')
    t = re.sub(rb'<position> 0 [0-9.]+ 0', b'<position> 0 %.2f 0' % (ys[0] + STEP * (k + 3)), t)
    new += t
x = x[:m.end()] + new + x[m.end():]
for k, name in enumerate((b'ControllerAdjust', b'TopTen')):
    x, n = re.subn(rb'(<text n="' + name + rb'">\s*<position> 0 )[0-9.]+( 0)', lambda mm: mm.group(1) + b'%.2f' % (ys[0] + STEP * (k + 1)) + mm.group(2), x)
    assert n == 1, name

# Pay the bytes back with indentation.
need = len(x) - orig_len
out = bytearray()
for c in x:
    if need > 0 and c == 9:
        need -= 1
        continue
    out.append(c)
assert need == 0 and len(out) == orig_len, (need, len(out), orig_len)
open(dst, 'wb').write(b[:i] + bytes(out) + b[j:])
print('Options entries added; interface length unchanged (%d bytes)' % orig_len)
