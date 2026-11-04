"""Slot maps for porting movesets: which SvR 2011 CHAR/PRO move slot each slot of another game's
profile corresponds to -> tools/moveset_maps.json (read by moveset_port.py).

    py moveset_maps.py <game> --src <extract> [--pac11 <2011 pac>] [--svrmod <exe>]

How: the superstars both games have (same id and name), their move ids translated to 2011's by
move name (MOVS/WAZE). For every (source slot, 2011 slot) pair the share of those superstars whose
moves agree; a monotonic alignment (the profiles keep their sections in the same order) picks the
pairs with the most agreement (dynamic programming, pairs under 15 % dropped), and a gap of the same
size on both sides between two pairs is filled one to one. 2010 has its own map (svr10_superstar MAP). Move names: MOVS/WAZE - '12 / '13 / 2K14 through
moves_port, SvR 2007-2009 in misc.pac (144-byte records, waze_old).
"""
import argparse
import collections
import json
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import moves_port as MP  # noqa: E402
import superstar_port as SP  # noqa: E402

N11 = 0x1C0 // 2          # 2011's move block: PRO +0x000-0x1BF, u16 move ids
OUT = os.path.join(HERE, 'moveset_maps.json')
SAME_IDS = ('svr2007', 'svr2008', 'svr2009')   # move ids are 2011's where 2011 has the move


def waze_old(src):
    """SvR 2007-2009 misc.pac MOVS/WAZE: 144-byte records from the table's start, name +0x10, id +0x88"""
    g = MP.read_pac(open(src.get('pac/misc.pac'), 'rb').read())
    raw = MP.unz(dict(((t, n), b) for t, es in g for n, b in es)[(b'MOVS', b'WAZE')])
    out = {}
    for p in range(0, len(raw) - 143, 144):
        nm = raw[p + 0x10:p + 0x50].split(b'\0')[0]
        if len(nm) > 2 and all(32 <= c < 127 for c in nm):
            out.setdefault(struct.unpack_from('<H', raw, p + 0x88)[0], nm.decode('latin1').strip())
    return out


def source_waze(gid, src, svrmod):
    if gid in ('svr2007', 'svr2008', 'svr2009', 'low'):   # (Legends of WrestleMania: 2009's table, its own ids)
        return waze_old(src)
    if gid == 'wwe12':
        return MP.Game12(src).waze
    if gid == 'wwe13':
        return MP.Game13(src, svrmod).waze
    if gid == 'wwe2k14':
        return MP.Game14(src, svrmod).waze
    if gid in ('wwe2k15', 'wwe2k16', 'wwe2k17'):
        return MP.GAME_CLASS[gid](src, svrmod).waze
    raise SystemExit('no move names for %s yet' % gid)


def profiles(g):
    """the source game's {id: CHAR/PRO record} (SvR 2007-2009 readers keep theirs in g.g)"""
    return g.pro if hasattr(g, 'pro') else g.g.pro


def fit(g, g11, trans, greedy=False, nt=N11):
    """g11: the target game (2011, or 2010 for the chain) - .pro, .name; nt: its slot count"""
    S = SP.svr10()
    pro = profiles(g)
    # (real superstars only: the Create-a-Superstar placeholders - "Edit 01".. below id 100 - match by name too)
    shared = [c for c in pro if 100 <= g.id11(c) < 400 and g.id11(c) in g11.pro and c in g.dat and g.named(c)
              and not g.name(c).upper().startswith('EDIT') and S.norm(g.name(c)) == S.norm(g11.name(g.id11(c)))]
    ns = min(len(pro[c]) for c in shared) // 2
    nt = min(nt, min(len(g11.pro[g.id11(c)]) for c in shared) // 2)
    N11_ = nt
    A = {c: struct.unpack_from('<%dH' % nt, g11.pro[g.id11(c)], 0) for c in shared}
    B = {c: struct.unpack_from('<%dH' % ns, pro[c], 0) for c in shared}
    cnt = np.zeros((ns, N11_))
    tot = np.zeros(ns)
    for c in shared:
        for so, v in enumerate(B[c]):
            t = trans(v)
            if not t:
                continue
            tot[so] += 1
            for to in range(N11_):
                if A[c][to] in t:
                    cnt[so, to] += 1
    score = cnt / (tot[:, None] + 2)
    if greedy:   # (SvR 2007-2009: sections reordered against 2011 - the strongest pairs first, one to one)
        m, used_t = {}, set()
        for so, to in sorted(zip(*np.nonzero(score >= 0.4)), key=lambda st: -score[st[0], st[1]]):
            if so in m or to in used_t or cnt[so, to] < 4:
                continue
            m[int(so)] = (int(to), round(float(score[so, to]), 2))
            used_t.add(to)
        return m, len(shared), ns
    score[score < 0.15] = 0
    D = np.zeros((ns + 1, N11_ + 1))
    P = np.zeros((ns + 1, N11_ + 1), dtype=np.int8)
    for i in range(1, ns + 1):
        for j in range(1, N11_ + 1):
            cand = (D[i - 1, j], D[i, j - 1], D[i - 1, j - 1] + score[i - 1, j - 1])
            k = int(np.argmax(cand))
            D[i, j], P[i, j] = cand[k], k
    pairs, i, j = [], ns, N11_
    while i > 0 and j > 0:
        k = P[i, j]
        if k == 2:
            if score[i - 1, j - 1] > 0:
                pairs.append((i - 1, j - 1, float(score[i - 1, j - 1])))
            i, j = i - 1, j - 1
        elif k == 0:
            i -= 1
        else:
            j -= 1
    pairs.reverse()
    m = {s: (t, round(sc, 2)) for s, t, sc in pairs}
    anch = sorted((s, t) for s, (t, _) in m.items())
    for (s0, t0), (s1, t1) in zip(anch, anch[1:]):
        if s1 - s0 == t1 - t0 and s1 - s0 > 1:
            for d in range(1, s1 - s0):
                m.setdefault(s0 + d, (t0 + d, 0.0))
    return m, len(shared), ns


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument('game')
    ap.add_argument('--src', required=True)
    ap.add_argument('--pac11', default=SP.PAC11 if hasattr(SP, 'PAC11') else MP.PAC11)
    ap.add_argument('--svrmod')
    ap.add_argument('--cache', help='where the files a disc image gives are copied (default tools/work/src/<game>)')
    ap.add_argument('--pac10', help='SvR 2010 pac (the chain for 2007-2009)')
    a = ap.parse_args(argv[1:])
    g11, _ = SP.game11(a.pac11)
    by11 = collections.defaultdict(set)
    for i, (bits, nm) in g11.waze().items():
        by11[MP.norm(nm)].add(i)
    src = SP.Src(a.src, a.cache or os.path.join(HERE, 'work', 'src', a.game))
    g = SP.open_game(a.game, src, a.svrmod)
    w = source_waze(a.game, src, a.svrmod)

    w11 = g11.waze()
    same_ids = a.game in SAME_IDS

    def trans(i):
        if same_ids and i in w11:   # (SvR 2007-2009 share 2011's move ids; names repeat across ids)
            return {i}
        nm = w.get(i)
        return by11.get(MP.norm(nm), set()) if nm else set()
    if same_ids:
        # (SvR 2007-2009: aligned with SvR 2010 - nearer, the same ids - and composed with the 2010 -> 2011
        # map, svr10_superstar.MAP; then the direct pairs with 2011 where both slots are still free)
        import svr10_superstar as SS
        if not a.pac10:
            ap.error('--pac10 is needed for %s (the chain through SvR 2010)' % a.game)
        g10 = SP.svr10().Game(a.pac10, 10)
        w10 = g10.waze()
        m10, nshared, ns = fit(g, g10, lambda i: {i} if i in w10 else set(), nt=4096)
        M10 = dict(SS.MAP)
        m = {so: (M10[to * 2] // 2, sc) for so, (to, sc) in m10.items() if to * 2 in M10}
        md, _, _ = fit(g, g11, trans)
        used = {t for t, _ in m.values()}
        for so, (to, sc) in md.items():
            if so not in m and to not in used:
                m[so] = (to, sc)
                used.add(to)
    else:
        m, nshared, ns = fit(g, g11, trans)
    data = json.load(open(OUT)) if os.path.exists(OUT) else {}
    data[a.game] = {'about': '%s PRO u16 slot (byte offset, hex) -> [2011 byte offset, agreement share (0 = gap filled)]; '
                             '%d shared superstars, %d source slots' % (a.game, nshared, ns),
                    'map': {'%03x' % (s * 2): ['%03x' % (t * 2), sc] for s, (t, sc) in sorted(m.items())}}
    with open(OUT, 'w', newline='\n') as f:
        json.dump(data, f, indent=1, sort_keys=True)
    print(json.dumps({'game': a.game, 'shared': nshared, 'mapped': len(m), 'covers': len({t for t, _ in m.values()})}))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
