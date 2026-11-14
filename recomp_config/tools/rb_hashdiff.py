"""Compare two rollback netplay hash logs (GZ_RB_HASHLOG) tick by tick.

Each log holds, per tick, a hash of every guest page that tick wrote, as it
was at the tick's end, and a mark at every rollback (the ticks from its
target on are then logged again). This replays both logs and reports where
the two sides' memory first differs: the earliest ticks at which a page's
contents differ, and whether the page later agrees again (a transient
difference, typically sound or timing bookkeeping) or stays different.

    py -3 rb_hashdiff.py hash1.bin hash2.bin [--first 40] [--from TICK]
"""
import argparse
import struct
import sys

WPAGES = 64 * 1024 * 1024 // 4096
MARK = 0xFFFFFFFF


def load(path):
    ticks = {}
    data = open(path, "rb").read()
    n = len(data) // 16
    rb = 0
    for k in range(n):
        t, page, lo, hi = struct.unpack_from("<4I", data, k * 16)
        if t == MARK:
            rb += 1
            for x in [x for x in ticks if x >= page]:
                del ticks[x]
            continue
        ticks.setdefault(t, {})[page] = lo | (hi << 32)
    return ticks, rb


def va(page):
    if page < WPAGES:
        return "%08X" % (page * 4096)
    if page < 2 * WPAGES:
        return "%08X" % (0x80000000 + (page - WPAGES) * 4096)
    return "gpu+%06X" % ((page - 2 * WPAGES) * 4096)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--first", type=int, default=40)
    ap.add_argument("--from", dest="start", type=int, default=0)
    o = ap.parse_args()
    ta, rba = load(o.a)
    tb, rbb = load(o.b)
    last = min(max(ta or [0]), max(tb or [0]))
    print("side A: %d ticks logged, %d rollbacks; side B: %d ticks, %d rollbacks; compared to tick %d"
          % (len(ta), rba, len(tb), rbb, last))
    cur_a, cur_b = {}, {}
    differ = {}        # page -> tick it began to differ
    events = []        # (tick, page, "differs" | "agrees again after N")
    persistent_at = {}
    for t in range(0, last + 1):
        ca, cb = ta.get(t, {}), tb.get(t, {})
        cur_a.update(ca)
        cur_b.update(cb)
        for p in set(ca) | set(cb):
            same = cur_a.get(p) == cur_b.get(p)
            if not same and p not in differ:
                differ[p] = t
                if t >= o.start:
                    events.append((t, p, "differs"))
            elif same and p in differ:
                if t >= o.start:
                    events.append((t, p, "agrees again after %d ticks" % (t - differ[p])))
                del differ[p]
    shown = 0
    for t, p, what in events:
        if what == "differs":
            print("tick %6d  page %s  %s" % (t, va(p), what))
            shown += 1
            if shown >= o.first:
                break
    print("pages still different at tick %d: %d" % (last, len(differ)))
    by_start = sorted(differ.items(), key=lambda kv: kv[1])
    for p, t in by_start[: o.first]:
        print("   since tick %6d  page %s" % (t, va(p)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
