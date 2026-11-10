"""Add a Biollante cell to the monster select grid.

The select screen's layout is XML text inside ShellData/ControlRoom.bdl
(<interface n="Monster Select">): Thumbnails > Row0 / Row1, one <o n="<monster>">
cell per monster, and the select script treats a cell by its name (a cell named
"biollante" selects her, with her portrait and name). The disc has no cell for
her. This writes a copy of the bundle with a 19th cell appended to Row0.

The bundle is a packed binary, so the XML must keep its exact length. Room is
made by dropping the 1-pixel nudge tags (<p> 0 -1 0</p>, <p> -1 -1 0</p>) inside
the cells' "locked" overlays and, for the last few bytes, single spaces after
<p> in other tags. Her cell shows the question-mark tile in green: the bundle
has no thumbnail of her.

Usage: make_biollante_slot.py <original ControlRoom.bdl> <output ControlRoom.bdl>
"""
import sys

src, dst = sys.argv[1], sys.argv[2]
SHIFT = int(sys.argv[3]) if len(sys.argv) > 3 else -27      # move the grid left to fit a 10th column
b = open(src, 'rb').read()
i = b.find(b'<interface n="Monster Select">')
j = b.find(b'</interface>', i)
assert i >= 0 and j > i, 'Monster Select interface not found'
x = b[i:j]
orig_len = len(x)
assert b'<o n="biollante">' not in x, 'already has a biollante cell'

Q = b'SOURCE\\MONSTERSELECT_EN_QUESTION'
cell = (b'<o n="biollante"><p> 571 0 0</p>'
        b'<q n="disabled"><f>0x1</f><b>' + Q + b'</b><c>0x406040FF</c></q>'
        b'<q n="enabled"><b>' + Q + b'</b><c>0x60FF60FF</c></q>'
        b'<q n="infocus"><f>0x1</f><b>' + Q + b'</b><c>0xC0FFC0FF</c></q>'
        b'<o n="locked"><f>0x1</f><q n="color"><b>' + Q + b'</b><c>0x80FF80FF</c></q>'
        b'<q n="question_mark"><b>SOURCE\\OUTLINE</b></q></o></o>')

# Append to Row0: right after the last Row0 cell (moguera2), before Row1 starts.
r1 = x.find(b'</o><o n="Row1">')
assert r1 > 0
x = x[:r1] + cell + x[r1:]

# Shift the whole grid left so ten columns fit inside the frame.
old_grp = b'<o n="Thumbnails"><p> 3 -7 0</p>'
assert x.count(old_grp) == 1
x = x.replace(old_grp, b'<o n="Thumbnails"><p> %d -7 0</p>' % (3 + SHIFT))

# Pay the bytes back.
need = len(x) - orig_len
t = x.find(b'<o n="Thumbnails">')
head, grid = x[:t], x[t:]
for tag in (b'<p> -1 -1 0</p>', b'<p> 0 -1 0</p>'):
    while need >= len(tag) and tag in grid:
        k = grid.find(tag)
        grid = grid[:k] + grid[k + len(tag):]
        need -= len(tag)
while need > 0 and b'<p> ' in head:
    k = head.find(b'<p> ')
    head = head[:k + 3] + head[k + 4:]
    need -= 1
x = head + grid
if need < 0:                                    # removed a little too much: pad a number
    k = x.find(b'<p>', x.find(b'<o n="biollante">'))
    x = x[:k + 3] + b' ' * (-need) + x[k + 3:]
    need = 0
assert need == 0 and len(x) == orig_len, (need, len(x), orig_len)
open(dst, 'wb').write(b[:i] + x + b[j:])
print('biollante cell added; interface length unchanged (%d bytes)' % orig_len)
