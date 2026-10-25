#!/usr/bin/env python3
"""Can a new disc run on an existing lift? Compare its default.xbe with the
lifted one and print the line for config/<game>/xbe_editions.h.

    tools/xbe_edition.py <new default.xbe> <lifted default.xbe> [edition]
    make xbe-edition GAME=remix XBE=game-remix-usa/default.xbe

A lift is the machine code of one XBE; another edition's disc can use it
when its code sections (the executable ones) are byte for byte the same at
the same addresses. Data sections may differ: the runtime loads them from
the disc's own XBE. What changes between editions is usually the header
(signature, certificate, digests). Then the new disc joins as an edition of
that lift: add the printed X(...) line to config/<game>/xbe_editions.h, a
title ID to config/<game>/app.env, and the edition to APP_EDITIONS.

Code that differs only in branch conditions (a jcc's opcode, same target)
can share the lift too, as an edition variant: the lifter emits both
conditions and the runtime picks one (config/<game>/edition_variants.json,
MC3_XBE_VARIANTS in xbe_editions.h; the tool prints both). Any other code
difference needs a lift of its own (docs/decisions/0005).

Found: the DUB Edition's European disc (2026-10-24), 531 header bytes
apart, code identical; the Remix's USA disc (2026-10-25), one branch apart
(ja -> jae at 0x001525FC).
"""
import struct
import sys

REGIONS = {1: "usa", 2: "jpn", 4: "eur"}


def fnv1a64(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return h


def parse(path):
    raw = open(path, "rb").read()
    if raw[:4] != b"XBEH":
        sys.exit("%s: not an XBE" % path)
    base, = struct.unpack_from("<I", raw, 0x104)
    cert, = struct.unpack_from("<I", raw, 0x118)
    nsec, secs_va = struct.unpack_from("<II", raw, 0x11C)
    region, = struct.unpack_from("<I", raw, cert - base + 0xA0)
    version, = struct.unpack_from("<I", raw, cert - base + 0xAC)
    title = raw[cert - base + 0xC:cert - base + 0xC + 80].decode("utf-16-le").split("\0")[0]
    secs = []
    for i in range(nsec):
        o = secs_va - base + i * 56
        flags, va, vsize, raddr, rsize, name_va = struct.unpack_from("<6I", raw, o)
        name = raw[name_va - base:name_va - base + 16].split(b"\0")[0].decode(errors="replace")
        secs.append((name, va, vsize, raw[raddr:raddr + rsize], bool(flags & 4)))
    return {"raw": raw, "region": region, "version": version, "title": title,
            "secs": secs, "fnv": fnv1a64(raw)}


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    new, old = parse(sys.argv[1]), parse(sys.argv[2])
    edition = sys.argv[3] if len(sys.argv) > 3 else REGIONS.get(new["region"], "region%x" % new["region"])
    print("new:    %s, %d bytes, region 0x%x, version 0x%x, FNV-1a 64 0x%016x"
          % (new["title"], len(new["raw"]), new["region"], new["version"], new["fnv"]))
    print("lifted: %s, %d bytes, region 0x%x, version 0x%x, FNV-1a 64 0x%016x"
          % (old["title"], len(old["raw"]), old["region"], old["version"], old["fnv"]))
    if new["fnv"] == old["fnv"]:
        print("\nthe same XBE: nothing to add")
        return 0
    olds = {(n, va): (vs, d, x) for n, va, vs, d, x in old["secs"]}
    code_diff, data_diff, missing, jccs = [], [], [], []
    for n, va, vs, d, x in new["secs"]:
        o = olds.pop((n, va), None)
        if o is None:
            missing.append("%s at 0x%08X only in the new XBE" % (n, va))
            continue
        if o[0] == vs and o[1] == d:
            continue
        idx = [i for i, (a, b) in enumerate(zip(o[1], d)) if a != b]
        if len(o[1]) != len(d) or o[0] != vs:
            missing.append("%s at 0x%08X: another size" % (n, va))
        elif not (x or o[2]) or n.startswith((".rdata", ".data")):
            # Data: the runtime loads it from the disc's own XBE (some XBEs
            # flag .rdata executable: the Remix's). Only what the lifter
            # turned into C has to match.
            data_diff.append("%-10s 0x%08X  %d bytes (data: loaded from the disc)" % (n, va, len(idx)))
        else:
            code_diff.append((n, va, d, o[1], idx))
    missing += ["%s at 0x%08X only in the lifted XBE" % (n, va) for n, va in olds]
    hdr = sum(1 for a, b in zip(new["raw"][:0x1000], old["raw"][:0x1000]) if a != b)
    print("\nheader (first 4 KB): %d bytes differ" % hdr)
    for line in data_diff:
        print("  " + line)

    # A code difference an edition variant covers: a short jcc (0x70-0x7F)
    # or a near one (0F 80-8F) whose opcode alone changed.
    jcc = "jo jno jb jae je jne jbe ja js jns jp jnp jl jge jle jg".split()
    other = []
    for n, va, d, o, idx in code_diff:
        for i in idx:
            near = i > 0 and d[i - 1] == 0x0F and o[i - 1] == 0x0F
            lo, hi = (0x80, 0x8F) if near else (0x70, 0x7F)
            if lo <= d[i] <= hi and lo <= o[i] <= hi:
                jccs.append((va + i - (1 if near else 0), jcc[o[i] - lo], jcc[d[i] - lo]))
            else:
                other.append("%-10s 0x%08X  %02X -> %02X" % (n, va + i, o[i], d[i]))
    if missing or other:
        print("code that differs:")
        for line in missing + other:
            print("  " + line)
        print("\n=> different code: this disc needs a lift of its own (docs/decisions/0005);"
              " it cannot join the existing one as an edition.")
        return 1
    if jccs:
        print("code: identical but for %d branch condition(s)" % len(jccs))
        for a, was, now in jccs:
            print("  0x%08X  %s (lifted disc) -> %s" % (a, was, now))
        print("\n=> the same code but for branches: add this edition as a variant.")
        print("   config/<game>/edition_variants.json (a new variant number if the edition has none):")
        for a, was, now in jccs:
            print('    {"addr": "0x%08X", "base": "%s", "%s": "%s"}' % (a, was, edition, now))
        print('   config/<game>/xbe_editions.h: MC3_XBE_VARIANTS(V) V("%s", <n>), and in MC3_XBE_EDITIONS:' % edition)
    else:
        print("code: every section identical")
        print("\n=> the same code: add this edition to the existing lift.")
        print("   config/<game>/xbe_editions.h, in MC3_XBE_EDITIONS:")
    print('    X("%s", 0x%016xull, 0x%xu, 0x%xu)   /* %s */ \\'
          % (edition, new["fnv"], new["region"], new["version"], new["title"]))
    print('   config/<game>/app.env: APP_EDITIONS += %s, and APP_TITLE_ID_%s (random, 01xxxxxxxxxxx000)'
          % (edition, edition.replace("-", "_")))
    if jccs:
        print("   then: make relift GAME=<game>")
    return 0


if __name__ == "__main__":
    sys.exit(main())
