"""fast_mmio.py <gen dir>

Post-process the lifted code after tools.recomp: in the DirectSound functions
that write the sound chip's registers every frame, 32-bit memory accesses go
through GZ_R32 / GZ_W32, which call the APU model directly for addresses in
its register window instead of faulting on them (each trapped access costs a
few microseconds of exception dispatch; with a dozen 3D voices that was about
a tenth of the game thread at 240 fps). Behaviour is unchanged: the trap path
ends in the same two calls. Safe to run twice.
"""
import glob
import os
import re
import sys

# CMcpxVoiceClient: HRTF filter commit, voice volume commit (addresses in the
# tested default.xbe; a function that isn't found is skipped).
FUNCS = ('sub_0025658E', 'sub_00256B42')

PRELUDE = '''
/* ---- fast_mmio.py: direct sound-chip register access for the functions below ---- */
#ifndef GZ_FAST_MMIO
#define GZ_FAST_MMIO
extern void     gz_apu_w32(uint32_t addr, uint32_t value);
extern uint32_t gz_apu_r32(uint32_t addr);
#define GZ_IS_APU(a) ((a) >= 0xFE800000u && (a) < 0xFE830000u)
static inline uint32_t gz_r32(uint32_t a) { return GZ_IS_APU(a) ? gz_apu_r32(a) : MEM32(a); }
#define GZ_R32(a) gz_r32((uint32_t)(a))
#define GZ_W32(a, v) do { uint32_t gz_a_ = (uint32_t)(a), gz_v_ = (uint32_t)(v); \\
    if (GZ_IS_APU(gz_a_)) gz_apu_w32(gz_a_, gz_v_); else MEM32(gz_a_) = gz_v_; } while (0)
#endif
'''


def close_paren(s, i):
    """Index of the parenthesis that closes the one at s[i]."""
    depth = 0
    for k in range(i, len(s)):
        if s[k] == '(':
            depth += 1
        elif s[k] == ')':
            depth -= 1
            if depth == 0:
                return k
    return -1


def convert(line):
    m = re.match(r'^(\s*)MEM32\(', line)
    if m:
        open_at = m.end() - 1
        end = close_paren(line, open_at)
        rest = line[end + 1:]
        m2 = re.match(r'^ = (.*);(\s*(/\*.*\*/)?\s*)$', rest, re.S)
        if end > 0 and m2:
            addr = line[open_at + 1:end]
            value = m2.group(1).replace('MEM32(', 'GZ_R32(')
            return '%sGZ_W32(%s, %s);%s' % (m.group(1), addr.replace('MEM32(', 'GZ_R32('), value, m2.group(2))
    return line.replace('MEM32(', 'GZ_R32(')


def process(path):
    src = open(path, encoding='utf-8', newline='').read()
    nl = '\r\n' if '\r\n' in src else '\n'
    lines = src.split(nl)
    out, done, i = [], [], 0
    while i < len(lines):
        m = re.match(r'^void (sub_[0-9A-F]{8})(_gen)?\(void\)\s*$', lines[i])
        if m and m.group(1) in FUNCS and i + 1 < len(lines) and lines[i + 1].startswith('{'):
            j = i
            while j < len(lines) and lines[j] != '}':
                j += 1
            body = lines[i:j + 1]
            if not any('GZ_R32(' in b or 'GZ_W32(' in b for b in body):
                body = [convert(b) for b in body]
                done.append(m.group(1))
            if 'GZ_FAST_MMIO' not in nl.join(out):
                out.extend(PRELUDE.strip('\n').split('\n'))
            out.extend(body)
            i = j + 1
            continue
        out.append(lines[i])
        i += 1
    if done:
        open(path, 'w', encoding='utf-8', newline='').write(nl.join(out))
    return done


def main():
    gen = sys.argv[1]
    found = []
    for path in sorted(glob.glob(os.path.join(gen, 'recomp_*.c'))):
        found += process(path)
    print('fast_mmio: converted %s' % (', '.join(found) if found else 'nothing (already converted or not found)'))


if __name__ == '__main__':
    main()
