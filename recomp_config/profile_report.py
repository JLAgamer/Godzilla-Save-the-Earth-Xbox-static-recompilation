"""profile_report.py <profile.txt> <exe .map> [top N]

Summarise a GZ_PROFILE sample file: per thread, the share of samples and the
hottest functions (resolved through the linker map; samples outside the exe
are grouped by module, e.g. ntdll = waiting).
"""
import bisect
import collections
import re
import sys

prof_path, map_path = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 12

base = 0x140000000
syms = []
for line in open(map_path, encoding='latin-1'):
    m = re.match(r'\s*Preferred load address is ([0-9a-fA-F]+)', line)
    if m:
        base = int(m.group(1), 16)
        continue
    m = re.match(r'\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s', line)
    if m:
        syms.append((int(m.group(2), 16), m.group(1)))
syms.sort()
addrs = [a for a, _ in syms]


def name_of(off):
    va = base + off
    i = bisect.bisect_right(addrs, va) - 1
    return syms[i][1] if i >= 0 else '?'


per_thread = collections.defaultdict(collections.Counter)
totals = collections.Counter()
header = ''
for line in open(prof_path):
    if line.startswith('#'):
        header = line.strip()
        continue
    tid, mod, off, n = line.split()
    n = int(n)
    key = name_of(int(off, 16)) if mod.lower().endswith('.exe') else '[%s]' % mod
    per_thread[tid][key] += n
    totals[tid] += n

print(header)
for tid, tot in totals.most_common():
    c = per_thread[tid]
    exe_share = sum(v for k, v in c.items() if not k.startswith('[')) * 100.0 / tot
    print('\nthread %s: %d samples, %.0f%% in the exe' % (tid, tot, exe_share))
    for k, v in c.most_common(top):
        print('  %5.1f%%  %s' % (v * 100.0 / tot, k))
