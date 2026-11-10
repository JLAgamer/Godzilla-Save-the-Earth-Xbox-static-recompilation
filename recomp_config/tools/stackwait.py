"""For every thread of a running recomp: when it is outside the exe (waiting in
the OS or a driver), which exe functions are on its stack. Shows who waits on
what.  Usage: stackwait.py <pid> <map> [samples=300] [stack depth=3]"""
import ctypes, sys, re, time, collections
from ctypes import wintypes as w
k = ctypes.WinDLL('kernel32', use_last_error=True); ps = ctypes.WinDLL('psapi')
pid = int(sys.argv[1]); mapf = sys.argv[2]; N = int(sys.argv[3]) if len(sys.argv) > 3 else 40
class TE(ctypes.Structure):
    _fields_ = [('dwSize', w.DWORD), ('cntUsage', w.DWORD), ('th32ThreadID', w.DWORD),
                ('th32OwnerProcessID', w.DWORD), ('tpBasePri', w.LONG), ('tpDeltaPri', w.LONG), ('dwFlags', w.DWORD)]
class CTX(ctypes.Structure):
    _pack_ = 16
    _fields_ = [('pad0', ctypes.c_ulonglong * 6), ('ContextFlags', w.DWORD), ('MxCsr', w.DWORD),
                ('seg', w.WORD * 6), ('EFlags', w.DWORD), ('Dr', ctypes.c_ulonglong * 6),
                ('Rax', ctypes.c_ulonglong), ('Rcx', ctypes.c_ulonglong), ('Rdx', ctypes.c_ulonglong),
                ('Rbx', ctypes.c_ulonglong), ('Rsp', ctypes.c_ulonglong), ('Rbp', ctypes.c_ulonglong),
                ('Rsi', ctypes.c_ulonglong), ('Rdi', ctypes.c_ulonglong), ('R', ctypes.c_ulonglong * 8),
                ('Rip', ctypes.c_ulonglong), ('rest', ctypes.c_ubyte * 1024)]
k.OpenProcess.restype = w.HANDLE; k.OpenThread.restype = w.HANDLE; k.CreateToolhelp32Snapshot.restype = w.HANDLE
hp = k.OpenProcess(0x0410, False, pid)
WALK = int(sys.argv[4]) if len(sys.argv) > 4 else 0
mods = (w.HMODULE * 256)(); need = w.DWORD()
ps.EnumProcessModules(w.HANDLE(hp), mods, ctypes.sizeof(mods), ctypes.byref(need))
base = mods[0]
class MI(ctypes.Structure): _fields_ = [('base', ctypes.c_void_p), ('size', w.DWORD), ('entry', ctypes.c_void_p)]
mi = MI(); ps.GetModuleInformation(w.HANDLE(hp), w.HMODULE(base), ctypes.byref(mi), ctypes.sizeof(mi))
syms = []; pref = None
for l in open(mapf, errors='replace'):
    m = re.match(r'\s*Preferred load address is ([0-9a-fA-F]+)', l)
    if m: pref = int(m.group(1), 16)
    m = re.match(r'\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s', l)
    if m: syms.append((int(m.group(2), 16), m.group(1)))
syms.sort(); addrs = [a for a, _ in syms]
import bisect
def name(ip):
    if not (base <= ip < base + mi.size): return 'outside-exe'
    a = ip - base + pref; i = bisect.bisect_right(addrs, a) - 1
    return '%s+0x%X' % (syms[i][1], a - syms[i][0]) if i >= 0 else hex(a)
snap = k.CreateToolhelp32Snapshot(4, 0); te = TE(); te.dwSize = ctypes.sizeof(TE); tids = []
ok = k.Thread32First(w.HANDLE(snap), ctypes.byref(te))
while ok:
    if te.th32OwnerProcessID == pid: tids.append(te.th32ThreadID)
    ok = k.Thread32Next(w.HANDLE(snap), ctypes.byref(te))
N = int(sys.argv[3]) if len(sys.argv) > 3 else 300
DEPTH = int(sys.argv[4]) if len(sys.argv) > 4 else 3
tot = collections.Counter(); inexe = collections.Counter(); wait = {t: collections.Counter() for t in tids}
top = {t: collections.Counter() for t in tids}
for _ in range(N):
    for t in tids:
        h = k.OpenThread(0x004A, False, t)
        if not h: continue
        if k.SuspendThread(w.HANDLE(h)) != 0xFFFFFFFF:
            c = CTX(); c.ContextFlags = 0x100003
            if k.GetThreadContext(w.HANDLE(h), ctypes.byref(c)):
                tot[t] += 1
                if base <= c.Rip < base + mi.size:
                    inexe[t] += 1; top[t][name(c.Rip).split('+')[0]] += 1
                else:
                    buf = (ctypes.c_ulonglong * 512)(); got = ctypes.c_size_t()
                    k.ReadProcessMemory(w.HANDLE(hp), ctypes.c_void_p(c.Rsp), buf, 4096, ctypes.byref(got))
                    fr = [name(v).split('+')[0] for v in buf if base <= v < base + mi.size]
                    seen = []
                    for f in fr:
                        if f not in seen: seen.append(f)
                        if len(seen) == DEPTH: break
                    wait[t][' < '.join(seen) or '(no exe frame)'] += 1
            k.ResumeThread(w.HANDLE(h))
        k.CloseHandle(w.HANDLE(h))
    time.sleep(0.005)
for t in sorted(tids, key=lambda t: -inexe[t])[:5]:
    print('thread %d: %d samples, %d%% in exe' % (t, tot[t], 100 * inexe[t] // max(1, tot[t])))
    print('   busy in:', ', '.join('%s %d' % kv for kv in top[t].most_common(8)))
    for kv in wait[t].most_common(8): print('   waiting %4d  %s' % (kv[1], kv[0]))
