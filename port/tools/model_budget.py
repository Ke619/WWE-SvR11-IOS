"""How much of the match's per-wrestler vertex memory a character model pac takes.

    py model_budget.py <chNNN.pac> [...]          -> one line per attire (JSON with --json)

In a match each wrestler's model gets a physical memory window of 768 KB (192
pages of 4 KB; the game's XMemAlloc allocator 128, e.g. 0x1B240000-0x1B300000).
The model loader (sub_826E73F8) makes a vertex buffer and an index buffer per
mesh, each its own allocation, so each rounds up to whole pages:
    vertex buffer: vertices x 32 bytes (one weight block) or 44 (2-4 blocks)
    index buffer:  indices x 2 bytes
over the JBOY models of the attire's entry (body 0x0, hair 0x2710, eyelashes
0x4e20, blood overlays 0x7530-0x7532 ...). Over 192 pages the next buffer gets
no memory and the game crashes loading the match (a converted SvR 2009 Umaga
needed about 200; stock models measured up to ~180).
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ch_tool  # noqa: E402
import jboy  # noqa: E402
import svrfmt  # noqa: E402

PAGE = 4096
# Since 2.1.6 a full window takes the game's fallback memory (src/model_mem.cpp, the big-model memory): a wrestler's
# model may take LIMIT pages (6.25 MB, about 130,000 vertices). Older games keep the window (OLD_LIMIT) and leave a
# bigger model out (no crash): such a mod says needs=NEEDS_VERSION in its manifest.
# Measured 27 Oct (src/caw_limits.h): six 1,489-page superstars in a 327k-vertex arena, 6-man, 60 fps at 1080p (about
# 40 at 4x supersampling with 2x AA, GPU-bound).
OLD_LIMIT = 192      # pages per wrestler before 2.1.6 (the window)
LIMIT = 1600         # pages per wrestler (the game's ModelPageLimit, src/caw_limits.h)
WARN = 1530          # near the limit
# One model part (body 0x0, hair 0x2710 ...) over 2,048 pages crashes the match's draw (sub_826E6AD0), whatever the
# total. LIMIT is below it, so a model within LIMIT is within this too; checked all the same should LIMIT ever grow.
PART_LIMIT = 1900
NEEDS_VERSION = '2.1.6'   # (the first version with the big-model memory: one place to change it)


def pages(n):
    return (n + PAGE - 1) // PAGE


def needs(path):
    """NEEDS_VERSION when an attire of the ch.pac at path takes more than the old window (OLD_LIMIT pages), else None"""
    try:
        worst = max((t for _, t, _ in model_pages(path)), default=0)
    except Exception:   # noqa: BLE001
        return None
    return NEEDS_VERSION if worst > OLD_LIMIT else None


def parts_over(parts):
    """the parts over PART_LIMIT pages ({child: pages} as attire_pages gives)"""
    return {k: p for k, p in parts.items() if p > PART_LIMIT}


def mesh_pages(s):
    stride = 32 if max(1, s.u5c[0]) == 1 else 44
    idx = sum(len(st[1]) for st in s.strips)
    return pages(len(s.verts) * stride) + pages(idx * 2)


def attire_pages(kids):
    total, parts = 0, {}
    for k in sorted(kids):
        d = kids[k]
        if d[:4] != b'JBOY':
            continue
        try:
            m = jboy.read(d)
        except Exception:   # noqa: BLE001
            continue
        p = sum(mesh_pages(s) for s in m.meshes)
        parts[hex(k)] = p
        total += p
    return total, parts


def model_pages(path):
    """-> [(entry name, pages, {child: pages})] for the wrestler (kind 2) entries"""
    d = open(path, 'rb').read()
    _, groups = ch_tool.epk8_read(d)
    out = []
    for typ, ents in groups:
        if typ != b'EMD ':
            continue
        for name, blob in ents:
            n = name.decode('latin1').strip('\0 ')
            if not n.endswith('2'):
                continue
            kids = {i: ch_tool.unpack(x) for i, x in svrfmt.pach_read(ch_tool.unpack(blob))}
            t, parts = attire_pages(kids)
            out.append((n, t, parts))
    return out


def main(argv):
    as_json = '--json' in argv
    files = [a for a in argv[1:] if a != '--json']
    res = []
    for f in files:
        for n, t, parts in model_pages(f):
            state = ('part over' if parts_over(parts) else 'over' if t > LIMIT else 'near' if t > WARN
                     else 'needs ' + NEEDS_VERSION if t > OLD_LIMIT else 'ok')
            res.append(dict(file=f, entry=n, pages=t, limit=LIMIT, part_limit=PART_LIMIT, state=state, parts=parts))
            if not as_json:
                print('%s %s: %d of %d pages (%s) %s' % (os.path.basename(f), n, t, LIMIT, state, parts))
    if as_json:
        print(json.dumps(res))


if __name__ == '__main__':
    main(sys.argv)
