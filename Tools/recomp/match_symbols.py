#!/usr/bin/env python3
"""Name the functions of the European DOL (GLMP01 rev 0) after the USA ones.

The lm-decomp configs (CC0) name every USA function and split the USA DOL into translation
units, but the European config (GLMP01_00) has mostly fn_<addr> names and no units. SDK and
JSystem code is the same in both releases except for addresses, so a function is matched when
its size and its instruction words with address fields masked are the same.

Usage:
  match_symbols.py <us main.dol> <us symbols.txt> <us splits.txt> <pal main.dol> <pal symbols.txt> <out map>

Out map lines: <pal addr> <us name> <us tu or -> <match kind>
"""
import bisect
import re
import struct
import sys
from collections import defaultdict

from dol import Dol, load_symbols, load_splits


def masked(words):
    out = []
    for w in words:
        op = w >> 26
        if op == 18:                     # b/bl: target is an address
            w &= 0xFC000003
        elif op in (14, 15, 24, 25) or 32 <= op <= 55:
            w &= 0xFFFF0000              # addi/addis/ori/oris and D-form loads/stores
        elif op == 16:                   # bc: keep (relative inside the function)
            pass
        out.append(w)
    return tuple(out)


def main():
    us_dol, us_sym, us_spl, pal_dol, pal_sym, out = sys.argv[1:7]
    U, P = Dol(us_dol), Dol(pal_dol)
    us_f, _ = load_symbols(us_sym)
    pal_f, _ = load_symbols(pal_sym)
    tu_of = load_splits(us_spl)

    def sig(d, a, s):
        return (s, masked(d.words(a, s // 4)))

    pal_by_sig = defaultdict(list)
    for a, (n, s) in pal_f.items():
        if s and P.has(a):
            pal_by_sig[sig(P, a, s)].append(a)
    us_by_sig = defaultdict(list)
    for a, (n, s) in us_f.items():
        if s and U.has(a):
            us_by_sig[sig(U, a, s)].append(a)

    result = {}
    ambiguous = 0
    # Functions the European config already names (GXInit, OSInit, ...): pair by name first.
    us_by_name = {}
    for a, (n, s) in us_f.items():
        us_by_name.setdefault(n, a)
    for pa, (n, s) in pal_f.items():
        if not n.startswith("fn_") and n in us_by_name:
            result[pa] = (n, tu_of(us_by_name[n]), "name")
    for k, us_addrs in us_by_sig.items():
        pals = pal_by_sig.get(k, [])
        if not pals:
            continue
        if len(us_addrs) == len(pals):
            # Same count: pair them in address order (layout order is kept between releases).
            for ua, pa in zip(sorted(us_addrs), sorted(pals)):
                result.setdefault(pa, (us_f[ua][0], tu_of(ua), "exact" if len(pals) == 1 else "ordered"))
        else:
            ambiguous += 1
    # Second pass: unmatched functions between two matched neighbours with the same size.
    pal_sorted = sorted(a for a in pal_f if pal_f[a][1])
    us_sorted = sorted(a for a in us_f if us_f[a][1])
    us_index = {a: i for i, a in enumerate(us_sorted)}
    rev = {}
    for pa, (name, tu, kind) in result.items():
        for ua in us_sorted:
            pass
    name_to_us = {us_f[a][0]: a for a in us_f}
    filled = 0
    for i, pa in enumerate(pal_sorted):
        if pa in result or i == 0 or i + 1 >= len(pal_sorted):
            continue
        prev, nxt = pal_sorted[i - 1], pal_sorted[i + 1]
        if prev in result and nxt in result:
            ua_prev = name_to_us.get(result[prev][0])
            ua_next = name_to_us.get(result[nxt][0])
            if ua_prev is None or ua_next is None:
                continue
            j = us_index[ua_prev]
            if j + 2 < len(us_sorted) and us_sorted[j + 2] == ua_next:
                ua = us_sorted[j + 1]
                if us_f[ua][1] == pal_f[pa][1]:
                    result[pa] = (us_f[ua][0], tu_of(ua), "neighbour")
                    filled += 1
    # Third pass (the European game was linked with a newer Dolphin SDK, so many SDK functions
    # changed): between two matched anchors, pair the unmatched functions by opcode similarity.
    import difflib

    def ops(d, a, s):
        return [w >> 26 if (w >> 26) != 31 else 0x100 | ((w >> 1) & 0x3FF) for w in d.words(a, s // 4)]

    anchors = sorted((name_to_us[result[pa][0]], pa) for pa in result if result[pa][0] in name_to_us)
    a_us = [x[0] for x in anchors]
    pal_unmatched = [a for a in pal_sorted if a not in result]
    matched_us = set(x[0] for x in anchors)
    fuzzy = 0
    for ua in us_sorted:
        if ua in matched_us:
            continue
        i = bisect.bisect_left(a_us, ua)
        if i == 0 or i >= len(anchors):
            continue
        lo_pal, hi_pal = anchors[i - 1][1], anchors[i][1]
        if hi_pal <= lo_pal or hi_pal - lo_pal > 0x20000:
            continue
        cands = [pa for pa in pal_unmatched[bisect.bisect_right(pal_unmatched, lo_pal):bisect.bisect_left(pal_unmatched, hi_pal)]
                 if pa not in result]
        if not cands:
            continue
        uo = ops(U, ua, us_f[ua][1])
        best = []
        for pa in cands:
            ps = pal_f[pa][1]
            if not (0.5 <= ps / max(4, us_f[ua][1]) <= 2.0):
                continue
            r = difflib.SequenceMatcher(None, uo, ops(P, pa, ps), autojunk=False).ratio()
            best.append((r, pa))
        best.sort(reverse=True)
        if best and best[0][0] >= 0.6 and (len(best) == 1 or best[0][0] - best[1][0] >= 0.1):
            result[best[0][1]] = (us_f[ua][0], tu_of(ua), "similar%.2f" % best[0][0])
            fuzzy += 1
    print("fuzzy matches:", fuzzy)
    with open(out, "w") as w:
        w.write("# PAL (GLMP01 rev 0) address -> USA (GLME01) name, unit, match kind.\n")
        w.write("# Generated by Tools/recomp/match_symbols.py from the lm-decomp configs (CC0).\n")
        for pa in sorted(result):
            n, tu, kind = result[pa]
            w.write("%08X %s %s %s\n" % (pa, n, tu or "-", kind))
    named = sum(1 for pa in result if not result[pa][0].startswith("fn_"))
    sdk = sum(1 for pa in result if result[pa][1] and re.match(r"(os|gx|vi|pad|dvd|ar|card|ai|dsp|si|exi|db|mtx)/", result[pa][1]))
    print("PAL functions %d, matched %d (named %d, SDK %d, neighbour %d), ambiguous signatures %d" % (
        len(pal_f), len(result), named, sdk, filled, ambiguous))


if __name__ == "__main__":
    main()
