"""Port SvR 2010 moves into SvR 2011 (m.pac motion banks + misc.pac move tables).

    python svr10_moves.py analyze <pac10> <pac11> <ids> [--out DIR]
    python svr10_moves.py build   <pac10> <pac11> <ids> [--out DIR] [--namelist] [--extra-bits FILE]
                     (FILE: lines "<id> <bit>,<bit>" OR'ed into that move's 2011 WAZE category bits)
    python svr10_moves.py verify  <pac11> [--out DIR]
    python svr10_moves.py fix     <pac10> <pac11> <mod.svrmod>... [--dry]
                     (a built superstar mod: the corner / victim-end conversions below, version +0.1)

<pac10>/<pac11> are the games' "pac" folders (read only). <ids> is a comma list
of move ids, or @file with one id per line. Everything is written under --out
(default ./out next to this script): m.pac, mpsp.pac, misc.pac (patched full copies),
plist360.arc + plist360_4x3.arc (the game folder's arcs with these 3 pacs refreshed),
banks/ (each patched bank, unpacked), movepack/ (manifest.json + payload files:
what a runtime loader needs to apply the same patch on top of a stock 2011
install), report.txt.

What a move is in 2011 (see re_moveport.md):
  m.pac   YMKs motion banks, keyed (id, x = phase/variant, y = track:
          0 attacker, 1 defender, 30-32/50-61 extra/prop/camera tracks).
          The same key lives in several bank families: MVMT/<category>
          (per-category banks), MOT/BMxx/n (complete set), MOT/RRxx (the
          sources the per-superstar match bank is gathered from), plus
          MOT/RU*/RURR (rumble) and CAF (create-a-finisher) for some moves.
          MOTP/GAME (2010: m.pac, 2011: mpsp.pac) = packed copy of the BM set that the game
          registers for matches (sub_82162FF0 -> sub_82161400); its records use a packed
          pose codec, so 2010 MOTP data is copied verbatim into the matching child.
  misc.pac MOVS/WAZE  move record (name, category bits = which moveset slots
          / lists the move may go in, params). 2011 kept records for the
          removed moves but cleared their category bits.
          WAZA/DATA  (identical copies: BATS/INIT and BATH/INIT children
          10, 11, 12)  child 0 "EXH" per-key move data (2010 32 bytes ->
          2011 36 bytes, +u32 appended), child 1 per-key sound events,
          child 2 "MBD".
Conversions applied to 2010 data:
  * YMKs event op 0x57 (87): 2010 always sets bit 7 of its argument,
    2011 never does (474/474 vs 550/550; 189 identical motions differ only
    by this) -> cleared.
  * EXH record: +u32 taken from the most similar record in the 2011 group.
  * Sitting-in-the-corner moves re-rooted to 2011's corner pose (corner_shift).
  * Victims ending on the mat with op 49 -> 0 chained to the down motion of
    the most similar stock move (down_end_fix; BM and MOTP copies).
  * WAZE category bits: from 2011 "donor" moves whose 2010 bits are equal.
"""
import collections
import hashlib
import json
import os
import pickle
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import svrfmt  # noqa: E402  (read-only import: EPAC/PACH/BPE helpers, svrmod.exe bpe)

RR_CAP = 16384000          # 2010 RR00-02 / SPE1 are all just under 16000 KiB [L]
SENTINEL_IDS = (0x7fff,)


# ------------------------------------------------------------------ containers

unpack = svrfmt.unbpe


def repack(raw, was_bpe):
    return svrfmt.bpe_pack(raw) if was_bpe else raw


def read_epac(path):
    d = open(path, 'rb').read()
    return svrfmt.epac_read(d)


def entry_get(groups, typ, name):
    for t, ents in groups:
        if t == typ:
            for n, blob in ents:
                if n == name:
                    return blob
    raise KeyError((typ, name))


def entry_set(groups, typ, name, blob):
    for gi, (t, ents) in enumerate(groups):
        if t == typ:
            for ei, (n, b) in enumerate(ents):
                if n == name:
                    ents[ei] = (n, blob)
                    return
    raise KeyError((typ, name))


def tree_get(blob, ids):
    """follow PACH child ids -> unpacked bytes"""
    raw = unpack(blob)
    for i in ids:
        raw = unpack(dict(svrfmt.pach_read(raw))[i])
    return raw


def tree_set(blob, ids, newraw):
    """replace the PACH descendant at ids with newraw (re-BPE where the original was BPE)"""
    if not ids:
        return repack(newraw, blob[:4] == b'BPE ')
    was = blob[:4] == b'BPE '
    raw = unpack(blob)
    ch = svrfmt.pach_read(raw)
    out = []
    for i, d in ch:
        out.append((i, tree_set(d, ids[1:], newraw) if i == ids[0] else d))
    return repack(svrfmt.pach_write(out), was)


def path_split(p):
    """'MOT/BM01/1' -> (b'MOT ', b'BM01', [1])"""
    f = p.split('/')
    return f[0].ljust(4).encode(), f[1].ljust(4).encode(), [int(x, 16) for x in f[2:]]


def walk_banks(groups, typ_filter=None):
    """yield (path, raw) for every YMKs/YMBs bank in an m.pac"""
    def rec(raw, path):
        if raw[:4] == b'PACH':
            for i, d in svrfmt.pach_read(raw):
                yield from rec(unpack(d), path + '/%x' % i)
        elif raw[:4] in (b'YMKs', b'YMBs'):
            yield path, raw
    for t, ents in groups:
        for n, blob in ents:
            yield from rec(unpack(blob), t.decode().strip() + '/' + n.decode().strip())


# ------------------------------------------------------------------ YMKs banks

def ydir(raw):
    n = struct.unpack_from('<I', raw, 0x110)[0]
    return 0x114 + 16 * n, [struct.unpack_from('<BBHIII', raw, 0x114 + 16 * i) for i in range(n)]


def key_of(e):
    return (e[2] << 16) | (e[1] << 8) | e[0]


def bank_entries(raw):
    """-> list of dict(y,x,id,off,frames,rt,data) in directory order (data = bytes up to next distinct offset)"""
    base, ents = ydir(raw)
    offs = sorted(set(e[3] for e in ents)) + [len(raw) - base]
    nxt = {o: offs[i + 1] for i, o in enumerate(offs[:-1])}
    return [dict(y=e[0], x=e[1], id=e[2], off=e[3], frames=e[4], rt=e[5],
                 data=raw[base + e[3]:base + nxt[e[3]]]) for e in ents]


def frames(d):
    p = f = 0
    recs = []
    while p < len(d):
        b = d[p]
        if b < 0x60:
            f += b
            p += 1
            continue
        if b == 0x60:
            sz = d[p + 1] | (d[p + 2] << 8)
            p += 3
        else:
            sz = b - 0x60
            p += 1
        recs.append((f, p, sz))
        p += sz
        f += 1
    if p != len(d):
        raise ValueError('frame stream overruns its data')
    return f, recs


def events(blk, T):
    out = []
    p = 0
    while p < len(blk):
        op = blk[p]
        n = T[op]
        if n == 0:
            return out, False
        out.append((op, blk[p:p + n]))
        p += n
    return out, p == len(blk)


def motion_info(d, T):
    """parse a YMKs motion -> dict(frames, poses Counter, ops Counter, refs set, ok, errors)"""
    info = dict(frames=0, poses=collections.Counter(), ops=collections.Counter(), refs=set(), errors=[], root=[])
    try:
        nf, recs = frames(d)
    except ValueError as ex:
        info['errors'].append(str(ex))
        return info
    info['frames'] = nf
    for f, p, sz in recs:
        r = d[p:p + sz]
        if r and r[0] & 0x80:
            n = r[0] & 0x7f
            ev, pose = r[1:1 + n], r[1 + n:]
            lst, ok = events(ev, T)
            if not ok:
                info['errors'].append('frame %d: event block does not parse with this T' % f)
            for op, b in lst:
                info['ops'][op] += 1
                if op in (49, 50, 51):
                    mid = b[1] | (b[2] << 8)
                    x = b[3] if op >= 50 else 0
                    y = b[4] if op == 51 else 0
                    info['refs'].add((mid, x, y))
        else:
            pose = r
        info['poses'][len(pose)] += 1
        if len(pose) >= 14 and pose[0] == 0x7d:
            info['root'].append(struct.unpack_from('<3f', pose, 2))
    return info


def convert_motion(d, T):
    """2010 -> 2011 event conversion (op 87: clear bit 7 of its argument). Returns (bytes, n changed)."""
    out = bytearray(d)
    nf, recs = frames(d)
    changed = 0
    for f, p, sz in recs:
        if sz and d[p] & 0x80:
            n = d[p] & 0x7f
            q, end = p + 1, p + 1 + n
            while q < end:
                op = d[q]
                ln = T[op]
                if ln == 0:
                    raise ValueError('unknown event op %d' % op)
                if op == 87 and out[q + 1] & 0x80:
                    out[q + 1] &= 0x7f
                    changed += 1
                q += ln
    return bytes(out), changed


def root_z(d):
    """the first root Z (pose record 0x7D: float x, y, z at +2) of a BM motion, or None"""
    nf, recs = frames(d)
    for f, p, sz in recs:
        r = d[p:p + sz]
        if r and r[0] & 0x80:
            r = r[1 + (r[0] & 0x7f):]
        if len(r) >= 14 and r[0] == 0x7d:
            return struct.unpack_from('<f', r, 10)[0]
    return None


def shift_root_z(d, dz):
    """every root Z of a BM motion + dz"""
    out = bytearray(d)
    nf, recs = frames(d)
    for f, p, sz in recs:
        q = p + (1 + (d[p] & 0x7f) if sz and d[p] & 0x80 else 0)
        if p + sz - q >= 14 and d[q] == 0x7d:
            struct.pack_into('<f', out, q + 10, struct.unpack_from('<f', d, q + 10)[0] + dz)
    return bytes(out)


# SvR 2011 moved the "sitting in the corner" pose 254 units (~25 cm) out of
# the corner: its idle 152 / slump 581 end at victim root Z -586, 2010's at
# -333, and every 2011 sitting-corner move was re-rooted to match. A 2010
# sitting-corner move (2010 WAZE +0x73 = 12) ported as is starts both
# wrestlers deeper in the corner than the 2011 pose: the victim's hips inside
# the post and pads (Umaga's Running Knee 5832 lifted Sabu above the
# turnbuckle). 2011 re-rooted its own copies by the same amount (running
# dropkick 3422 x=20: victim -326 -> -577, attacker +251). So: victim (y 1/51)
# root Z - 253, attacker (y 0/50) + 253, per x (a running move's x=20 keys;
# its run-up x=0 is left alone) - when the victim starts where 2010's sitting
# pose is (first root Z above -450; 2010 has a few moves already at 2011's).
SIT_SITUATION, SIT_SHIFT, SIT_2010_MIN = 12, -253.0, -450.0


def corner_shift(motions):
    """motions: the dict(x, y, data) of one sitting-corner move (any banks) -> shifted in place; notes"""
    notes = []
    xs = set(m['x'] for m in motions)
    for x in sorted(xs):
        if x == 0 and 20 in xs:
            continue
        victim = next((m['data'] for m in motions if m['x'] == x and m['y'] == 1), None)
        z0 = root_z(victim) if victim else None
        if z0 is None or z0 < SIT_2010_MIN:
            continue
        d = SIT_SHIFT
        for m in motions:
            if m['x'] == x and m['y'] in (0, 1, 50, 51):
                m['data'] = shift_root_z(m['data'], d if m['y'] in (1, 51) else -d)
        notes.append('x=%d: victim root z %.0f -> %.0f (attacker %+.0f)' % (x, z0, z0 + d, -d))
    return notes


def event_blocks(d, motp=False):
    """(start, end) of each frame's event block in a motion: BM records start with
    0x80 | length, MOTP (packed) records with a byte below 0x40 = length - 1"""
    nf, recs = frames(d)
    out = []
    for f, p, sz in recs:
        if not sz:
            continue
        if not motp and d[p] & 0x80:
            out.append((p + 1, p + 1 + (d[p] & 0x7f)))
        elif motp and d[p] < 0x40:
            out.append((p + 1, p + 2 + d[p]))
    return out


def end_chain(d, T, motp=False):
    """the last op 49 (the motion that follows this one): (offset of its u16, target) or (None, None)"""
    at = tgt = None
    for q, end in event_blocks(d, motp):
        end = min(end, len(d))
        while q < end:
            op = d[q]
            ln = T[op]
            if ln == 0 or q + ln > end:
                break
            if op == 49:
                at, tgt = q + 1, struct.unpack_from('<H', d, q + 1)[0]
            q += ln
    return at, tgt


def last_root_y(d):
    nf, recs = frames(d)
    y = None
    for f, p, sz in recs:
        q = p + (1 + (d[p] & 0x7f) if sz and d[p] & 0x80 else 0)
        if p + sz - q >= 14 and d[q] == 0x7d:
            y = struct.unpack_from('<f', d, q + 6)[0]
    return y


# The victim's end. A victim track (y 1) ends with op 49 naming the motion
# that follows: 2011's victims that end on the mat name a down motion (443,
# 446, 448, 476...: lying, selling); a 2010 one often names 0, and in 2011
# that's straight to the get-up (Jeff's Hurricanrana 8, 7573: the opponent
# up ~250 frames sooner than after the stock Hurricanrana 5, 7450, whose
# victim goes to 446). So a ported victim that ends low (last root Y above
# END_LOW_Y) with op 49 -> 0 gets the target of the most similar stock 2011
# move: same EXH group and x, fewest differing EXH bytes, its victim chained
# to something (StockEnds).
END_LOW_Y = -400.0


class StockEnds:
    """2011: each victim track's end chain ((id, x) -> target, when not 0) and the EXH groups"""

    def __init__(self, pac11, cache):
        if os.path.exists(cache):
            got = pickle.load(open(cache, 'rb'))
            if len(got) == 3:   # (an older cache has no ids: made again)
                self.ends, self.exh, self.ids = got
                return
        self.ends = {}
        self.ids = set()   # every motion id 2011's m.pac / mpsp.pac banks have (missing_end_fix)
        for pac in ('m.pac', 'mpsp.pac'):
            if not os.path.exists(os.path.join(pac11, pac)):
                continue
            h, g, t = read_epac(os.path.join(pac11, pac))
            for path, raw in walk_banks(g):
                if raw[:4] in (b'YMKs', b'YMBs') and len(raw) >= 0x114:
                    n = struct.unpack_from('<I', raw, 0x110)[0]
                    for i in range(n):
                        if 0x114 + 16 * i + 4 <= len(raw):
                            self.ids.add(struct.unpack_from('<H', raw, 0x114 + 16 * i + 2)[0])
        h, g, t = read_epac(os.path.join(pac11, 'm.pac'))
        for path, raw in walk_banks(g):
            if not path.startswith('MOT/BM') or raw[:4] != b'YMKs':
                continue
            T = raw[0x10:0x110]
            for e in bank_entries(raw):
                if e['y'] != 1 or e['x'] >= 90 or (e['id'], e['x']) in self.ends:
                    continue
                try:
                    at, tgt = end_chain(e['data'], T)
                except (ValueError, IndexError):
                    continue
                if tgt:
                    self.ends[(e['id'], e['x'])] = tgt
        h, g, t = read_epac(os.path.join(pac11, 'misc.pac'))
        self.exh = {gid: exh_parse(u, 36) for gid, pk, u in subgroups(waza_children(g)[0])}
        os.makedirs(os.path.dirname(cache) or '.', exist_ok=True)
        pickle.dump((self.ends, self.exh, self.ids), open(cache, 'wb'))

    def donor(self, group, rec):
        """the most similar stock record's (id, x) and its victim's end target, or None"""
        cands = [r for r in self.exh.get(group, []) if r[2] == rec[2] and (rkey(r)[0], r[2]) in self.ends
                 and rkey(r)[0] != rkey(rec)[0]]
        if not cands:
            return None
        best = min(cands, key=lambda q: sum(a != b for a, b in zip(q[2:32], rec[2:32])))
        k = (rkey(best)[0], best[2])
        return k, self.ends[k]


def down_end_fix(motions, exh, stock, T):
    """motions: dict(id, x, y, data, motp) of one move; exh: {x: (2011 group, record)}.
    Victim tracks ending low with op 49 -> 0 get the donor's target (in place). -> notes"""
    notes = []
    for x in sorted(set(m['x'] for m in motions)):
        bm = next((m for m in motions if m['x'] == x and m['y'] == 1 and not m['motp']), None)
        if bm is not None and x not in exh:   # (a record 2011 kept: not in the pack)
            exh[x] = next(((g, r) for g, rs in stock.exh.items() for r in rs
                           if rkey(r)[0] == bm['id'] and r[2] == x), None)
        if bm is None or not exh.get(x):
            continue
        at, tgt = end_chain(bm['data'], T)
        y = last_root_y(bm['data'])
        if at is None or tgt != 0 or y is None or y <= END_LOW_Y:
            continue
        d = stock.donor(*exh[x])
        if d is None:
            continue
        (did, dx), new = d
        for m in motions:
            if m['x'] != x or m['y'] not in (1, 51):
                continue
            at, tgt = end_chain(m['data'], T, m['motp'])
            if at is not None and tgt == 0:
                b = bytearray(m['data'])
                struct.pack_into('<H', b, at, new)
                m['data'] = bytes(b)
        notes.append('x=%d: victim end 0 -> %d (from %d/%d; victim ends at root y %.0f)' % (x, new, did, dx, y))
    return notes


def missing_end_fix(motions, known, T):
    """motions: dict(id, x, y, data, motp). A track whose end chain (the last op 49) names a motion that neither
    2011 nor the pack has (`known`: their ids) - a 2010 / 2008 move ported as is: 869 in 5786 / 5768 / 5697, 170 in
    Sabu's 5790 - left the character in no motion: lying until a state reset stood him up ("the mod falls to the
    floor, then teleports standing", 23 Oct). Set to 0 (the game's own follow-up), in place. -> notes"""
    notes = []
    for m in motions:
        try:
            at, tgt = end_chain(m['data'], T, m['motp'])
        except (ValueError, IndexError):
            continue
        # (y 50+ tracks and ids that high don't read as this layout - Sabu's packed y 52 copies gave 47927, 64047:
        # left as they are)
        if at is None or not tgt or tgt in known or m['y'] >= 50 or tgt >= 20000:
            continue
        b = bytearray(m['data'])
        struct.pack_into('<H', b, at, 0)
        m['data'] = bytes(b)
        notes.append('x=%d y=%d%s: goes on to motion %d, which 2011 doesn\'t have -> 0' % (
            m['x'], m['y'], ' (packed copy)' if m['motp'] else '', tgt))
    return notes


def bank_insert(raw, new):
    """Insert motions into a YMKs bank. new: list of dict(y,x,id,frames,data).
    Keys already present are skipped. Directory stays sorted by (id,x,y); data is
    laid out in directory order (the match-bank builder sub_8224D8C8 sizes an entry
    as next.off - off, so offsets must stay monotonic). Returns (raw, added keys)."""
    if raw[:4] != b'YMKs':
        raise ValueError('only YMKs banks are supported')
    ents = bank_entries(raw)
    keys = [key_of((e['y'], e['x'], e['id'])) for e in ents]
    for i in range(len(ents) - 1):
        if ents[i]['off'] > ents[i + 1]['off']:
            raise ValueError('bank offsets not monotonic at %d' % i)
    # a few stock banks repeat a run of keys (e.g. BM01/4 has 6229 twice), so the
    # directory is only mostly sorted: insert before the first larger key
    present = set(keys)
    rt = collections.Counter(e['rt'] for e in ents).most_common(1)[0][0]
    merged = list(ents)
    added = []
    import bisect
    for m in sorted(new, key=lambda m: key_of((m['y'], m['x'], m['id']))):
        k = key_of((m['y'], m['x'], m['id']))
        if k in present:
            continue
        if m['id'] >= ents[-1]['id']:
            raise ValueError('new id above the bank sentinel')
        pos = next(i for i, kk in enumerate(keys) if kk > k)
        if pos and keys[pos - 1] > k:
            raise ValueError('cannot place key %r' % (k,))
        keys.insert(pos, k)
        merged.insert(pos, dict(y=m['y'], x=m['x'], id=m['id'], off=None, frames=m['frames'], rt=rt, data=m['data']))
        present.add(k)
        added.append((m['id'], m['x'], m['y']))
    body = bytearray()
    dirb = bytearray()
    last = None          # (original offset, new offset) of the previous entry
    for e in merged:
        # stock banks let neighbouring keys share one data block (same offset);
        # keep that only while the run is unbroken, else copy the block so offsets
        # stay monotonic (sizes are taken as next.off - off)
        if e['off'] is not None and last is not None and last[0] == e['off']:
            off = last[1]
        else:
            off = len(body)
            body += e['data']
        last = (e['off'], off)
        dirb += struct.pack('<BBHIII', e['y'], e['x'], e['id'], off, e['frames'], e['rt'])
    out = raw[:0x110] + struct.pack('<I', len(merged)) + bytes(dirb) + bytes(body)
    return out, added


# ------------------------------------------------------------------ indexes

def bank_index(pacdir, cache):
    """path -> (kind, set of keys (id,x,y), size)"""
    if os.path.exists(cache):
        try:   # (a cache cut short - a cancelled convert - is rebuilt)
            return pickle.load(open(cache, 'rb'))
        except Exception:   # noqa: BLE001
            pass
    idx = {}
    # 2011 moved MOTP/GAME (the packed in-match motion set) from m.pac to mpsp.pac
    for pacname in ('m.pac', 'mpsp.pac'):
        if not os.path.exists(os.path.join(pacdir, pacname)):
            continue
        h, groups, tr = read_epac(os.path.join(pacdir, pacname))
        for path, raw in walk_banks(groups):
            if pacname == 'mpsp.pac' and not path.startswith('MOTP'):
                continue
            base, ents = ydir(raw)
            idx[path] = (raw[:4].decode(), set((e[2], e[1], e[0]) for e in ents if e[2] not in SENTINEL_IDS), len(raw))
    tmp = cache + '.part'
    pickle.dump(idx, open(tmp, 'wb'))
    os.replace(tmp, cache)   # (written whole or not at all: a cancel can't leave half a cache)
    return idx


def family(path):
    f = path.split('/')
    if f[0] == 'MVMT':
        return 'MVMT'
    if f[0] == 'MOT':
        if f[1][:2] in ('BM', 'RR', 'RB', 'RU', 'BA', 'WE') and f[1][2:].isdigit():
            return 'MOT/' + f[1][:2]
        return 'MOT/' + f[1]
    return f[0]


def map_bank(p10, idx10, idx11, rr_choice):
    """2010 bank path -> 2011 bank path (largest key overlap within the same family and kind)"""
    fam = family(p10)
    if fam == 'MOT/RR':
        return rr_choice
    kind, keys10, _ = idx10[p10]
    best, bn = None, 0
    for p11, (k11, keys11, _) in idx11.items():
        if k11 != kind or family(p11) != fam:
            continue
        n = len(keys10 & keys11)
        if n > bn:
            best, bn = p11, n
    return best


# ------------------------------------------------------------------ misc.pac tables

def waza_children(groups):
    raw = unpack(entry_get(groups, b'WAZA', b'DATA'))
    return {i: d for i, d in svrfmt.pach_read(raw)}


def bats_children(groups, name=b'BATS'):
    raw = unpack(entry_get(groups, name, b'INIT'))
    return {i: d for i, d in svrfmt.pach_read(raw)}


def subgroups(blob):
    """PACH of per-group tables -> list of (id, packed bytes, unpacked bytes)"""
    raw = unpack(blob)
    return [(i, d, unpack(d)) for i, d in svrfmt.pach_read(raw)]


def exh_parse(u, rs):
    if u[:4] != b'EXH\0':
        return []
    n = struct.unpack_from('<I', u, 4)[0]
    return [u[8 + rs * k:8 + rs * (k + 1)] for k in range(n)]


def exh_build(recs):
    return b'EXH\0' + struct.pack('<I', len(recs)) + b''.join(recs)


def evt_parse(u):
    if len(u) < 8:
        return None
    rs, n = struct.unpack_from('<II', u, 0)
    if rs != 16:
        return None
    recs = [u[8 + 16 * k:8 + 16 * (k + 1)] for k in range(n)]
    ev = u[8 + 16 * n:]
    return recs, ev


def evt_list(rec, ev):
    i = struct.unpack_from('<H', rec, 8)[0]
    c = rec[10]
    return [ev[4 * (i + j):4 * (i + j + 1)] for j in range(c)]


def rkey(r):
    return (struct.unpack_from('<H', r, 0)[0], r[2], r[3])


def idkey(r):
    """The game finds WAZA records by a binary search on the u16 move id alone
    (sub_826A4960); its groups are in id order, not always in rkey order (EXH
    group 16: 845/0/1 before 845/0/0). Records go in by id (after the id's
    own): appended ones after the last id were never found, nor every later
    stock id (no hit reaction / impact sound on strikes)."""
    return struct.unpack_from('<H', r, 0)[0]


def sorted_insert(recs, new, keyf):
    import bisect
    keys = [keyf(r) for r in recs]
    for r in new:
        k = keyf(r)
        pos = bisect.bisect_right(keys, k)
        keys.insert(pos, k)
        recs.insert(pos, r)
    return recs


def is_sorted(recs, keyf):
    ks = [keyf(r) for r in recs]
    return all(ks[i] <= ks[i + 1] for i in range(len(ks) - 1))


def group_map(sub10, sub11, parse10, parse11):
    """2010 group id -> 2011 group id, by shared record keys"""
    k11 = {}
    for g, _, u in sub11:
        for r in parse11(u):
            k11.setdefault(rkey(r), collections.Counter())[g] += 1
    out = {}
    for g, _, u in sub10:
        c = collections.Counter()
        for r in parse10(u):
            for g11, n in k11.get(rkey(r), {}).items():
                c[g11] += n
        if c:
            out[g] = c.most_common(1)[0][0]
    return out


def waze_load(raw):
    """MOVS/WAZE -> (records start, count, {id: [record offsets]})"""
    st = raw.index(b'* test motion *') - 0x10
    n = (len(raw) - st) // 160
    ids = {}
    for k in range(n):
        o = st + 160 * k
        r = raw[o:o + 160]
        if len(r) < 160 or not r.strip(b'\0'):
            continue
        ids.setdefault(struct.unpack_from('<H', r, 0x90)[0], []).append(o)
    return st, n, ids


# ------------------------------------------------------------------ main logic

class Ctx:
    def __init__(self, pac10, pac11, out):
        self.pac10, self.pac11, self.out = pac10, pac11, out
        os.makedirs(os.path.join(out, 'cache'), exist_ok=True)
        self.log = []
        self.idx10 = bank_index(pac10, os.path.join(out, 'cache', 'idx10.pkl'))
        self.idx11 = bank_index(pac11, os.path.join(out, 'cache', 'idx11.pkl'))
        self.h10, self.g10, self.tr10 = read_epac(os.path.join(pac10, 'm.pac'))
        self.h11, self.g11, self.tr11 = read_epac(os.path.join(pac11, 'm.pac'))
        self.ph11, self.pg11, self.ptr11 = read_epac(os.path.join(pac11, 'mpsp.pac'))
        self.mh10, self.mg10, self.mtr10 = read_epac(os.path.join(pac10, 'misc.pac'))
        self.mh11, self.mg11, self.mtr11 = read_epac(os.path.join(pac11, 'misc.pac'))
        self.keys11 = set().union(*[v[1] for v in self.idx11.values()])
        self.ids11 = set(k[0] for k in self.keys11)
        self.ids10 = set(k[0] for v in self.idx10.values() for k in v[1])
        self._bank = {}

    def p(self, *a):
        s = ' '.join(str(x) for x in a)
        print(s)
        self.log.append(s)

    def bank(self, game, path):
        k = (game, path)
        if k not in self._bank:
            g = self.g10 if game == 10 else (self.pg11 if path.startswith('MOTP') else self.g11)
            t, n, ids = path_split(path)
            self._bank[k] = tree_get(entry_get(g, t, n), ids)
        return self._bank[k]

    def T11(self):
        return self.bank(11, 'MVMT/CORN/f')[0x10:0x110]

    def T10(self):
        return self.bank(10, 'MVMT/CORN/c')[0x10:0x110]

    def keys10_of(self, mid):
        """-> {path: [(id,x,y)]} for every 2010 bank holding motion id"""
        out = {}
        for p, (kind, keys, _) in self.idx10.items():
            ks = sorted(k for k in keys if k[0] == mid)
            if ks:
                out[p] = ks
        return out

    def rr_choice(self):
        best = None
        for p, (kind, keys, size) in self.idx11.items():
            if family(p) == 'MOT/RR' and p != 'MOT/RRBS' and kind == 'YMKs':
                if best is None or size < self.idx11[best][2]:
                    best = p
        return best

    # --- exh / events from 2010
    def waza10(self):
        if not hasattr(self, '_w10'):
            b = bats_children(self.mg10)
            self._w10 = {c: subgroups(b[c]) for c in (10, 11, 12)}
            w = waza_children(self.mg11)
            self._w11 = {c: subgroups(w[c - 10]) for c in (10, 11, 12)}
            self._gm = {
                10: group_map(self._w10[10], self._w11[10], lambda u: exh_parse(u, 32), lambda u: exh_parse(u, 36)),
                11: group_map(self._w10[11], self._w11[11], lambda u: (evt_parse(u) or ([], b''))[0], lambda u: (evt_parse(u) or ([], b''))[0]),
            }
        return self._w10, self._w11, self._gm


def parse_ids(s):
    if s.startswith('@'):
        return [int(l.split()[0]) for l in open(s[1:]) if l.strip() and not l.startswith('#')]
    return [int(x) for x in s.split(',') if x.strip()]


def analyze(ctx, ids):
    """per move: 2010 keys and banks, 2011 presence, compatibility checks, closure"""
    T10, T11 = ctx.T10(), ctx.T11()
    w10, w11, gm = ctx.waza10()
    todo = list(ids)
    seen = set()
    plan = collections.OrderedDict()
    while todo:
        mid = todo.pop(0)
        if mid in seen:
            continue
        seen.add(mid)
        locs = ctx.keys10_of(mid)
        allkeys = sorted(set(k for ks in locs.values() for k in ks))
        missing = [k for k in allkeys if k not in ctx.keys11]
        item = dict(id=mid, banks10=locs, keys=allkeys, missing=missing, motions={}, refs=set(), exh=[], evt=[], mbd=[])
        ctx.p('== move %d: %d keys in 2010, %d missing in 2011%s' % (mid, len(allkeys), len(missing), '' if mid in ids else ' (pulled in by a reference)'))
        for p, ks in sorted(locs.items()):
            ctx.p('   2010 %-14s -> 2011 %-14s %s' % (p, map_bank(p, ctx.idx10, ctx.idx11, ctx.rr_choice()), ' '.join('(%d,%d,%d)' % k for k in ks)))
        # motion data: take each key from its first non-MOTP 2010 bank; check all copies agree
        for k in allkeys:
            datas = {}
            for p, ks in locs.items():
                if k in ks and not p.startswith('MOTP'):
                    raw = ctx.bank(10, p)
                    for e in bank_entries(raw):
                        if (e['id'], e['x'], e['y']) == k:
                            datas[p] = e
                            break
            if not datas:
                continue
            hs = set(hashlib.md5(e['data']).hexdigest() for e in datas.values())
            e = next(iter(datas.values()))
            i10 = motion_info(e['data'], T10)
            i11 = motion_info(e['data'], T11)
            bad208 = i10['ops'].get(208, 0)
            conv, nconv = convert_motion(e['data'], T10)
            item['motions'][k] = dict(frames=e['frames'], data=e['data'], conv=conv, nconv=nconv, copies=len(datas), variants=len(hs),
                                      ops=dict(i11['ops']), poses=dict(i11['poses']), errors=i11['errors'], op208=bad208)
            ctx.p('   key %-14s frames %3d size %6d copies %d (%s) poses %s ops %s op87 fixed %d%s%s' % (
                str(k), e['frames'], len(e['data']), len(datas), 'identical' if len(hs) == 1 else '%d variants' % len(hs),
                dict(i11['poses']), sorted(i11['ops']), nconv,
                ' ERR ' + ';'.join(i11['errors'][:2]) if i11['errors'] else '', ' OP208!' if bad208 else ''))
            for r in i11['refs']:
                if r[0] != mid:
                    item['refs'].add(r)
            if i11['root']:
                xs = [v for t in i11['root'] for v in t]
                ctx.p('       root xyz range %.1f..%.1f, max step %.1f' % (min(xs), max(xs), max(
                    [max(abs(a - b) for a, b in zip(i11['root'][j], i11['root'][j + 1])) for j in range(len(i11['root']) - 1)] or [0])))
        # misc tables (2010 BATS/INIT 10, 11, 12)
        for g, _, u in w10[10]:
            for r in exh_parse(u, 32):
                if rkey(r)[0] == mid:
                    item['exh'].append((g, r))
                    for off in range(20, 32, 2):
                        v = struct.unpack_from('<H', r, off)[0]
                        if v and v != mid and v in ctx.ids10:
                            item['refs'].add((v, None, None))
        for g, _, u in w10[11]:
            pr = evt_parse(u)
            if pr:
                for r in pr[0]:
                    if rkey(r)[0] == mid:
                        item['evt'].append((g, r, evt_list(r, pr[1])))
        for g, _, u in w10[12]:
            if u[:4] == b'MBD\0':
                n = struct.unpack_from('<I', u, 4)[0]
                for k in range(n):
                    r = u[8 + 8 * k:16 + 8 * k]
                    if struct.unpack_from('<H', r, 0)[0] == mid:
                        item['mbd'].append((g, r))
        ctx.p('   WAZA: %d EXH records (2010 groups %s -> 2011 %s), %d sound-event records, %d MBD records' % (
            len(item['exh']), sorted(set(g for g, _ in item['exh'])), sorted(set(gm[10].get(g) for g, _ in item['exh'])),
            len(item['evt']), len(item['mbd'])))
        for r in sorted(item['refs'], key=str):
            st = 'in 2011' if (r[0] in ctx.ids11) else ('2010 only -> ported too' if r[0] in ctx.ids10 else 'not in either')
            ctx.p('   references motion %s: %s' % (r, st))
            if r[0] not in ctx.ids11 and r[0] in ctx.ids10 and r[0] not in seen:
                todo.append(r[0])
        plan[mid] = item
    return plan


def waze_recount(wz):
    """The header's 128 category counts (u16 LE at 8 + 2*cat) from the records' bits
    (u64 LE at +0 / +8, records of 160 bytes from 272, count u32 LE at 4). The game
    sizes its move lists by them (sub_8237BA68): stale counts overran the lists."""
    n = struct.unpack_from('<I', wz, 4)[0]
    cnt = [0] * 128
    for k in range(n):
        lo, hi = struct.unpack_from('<QQ', wz, 272 + 160 * k)
        for c in range(128):
            if ((lo if c < 64 else hi) >> (c % 64)) & 1:
                cnt[c] += 1
    struct.pack_into('<128H', wz, 8, *[min(c, 0xFFFF) for c in cnt])


def waze_flags(ctx, plan, requested=None):
    """2011 WAZE category bits for each ported id, from donors with equal 2010 bits"""
    w10 = unpack(entry_get(ctx.mg10, b'MOVS', b'WAZE'))
    w11 = unpack(entry_get(ctx.mg11, b'MOVS', b'WAZE'))
    st10, n10, i10 = waze_load(w10)
    st11, n11, i11 = waze_load(w11)
    f10 = {m: w10[o[0]:o[0] + 16] for m, o in i10.items()}
    f11 = {m: w11[o[0]:o[0] + 16] for m, o in i11.items()}
    res = {}
    for mid in plan:
        if requested is not None and mid not in requested:
            ctx.p('WAZE %d: pulled in by a reference only -> category bits left as they are' % mid)
            continue
        if mid not in f10 or mid not in f11:
            ctx.p('WAZE %d: no record in %s' % (mid, '2010' if mid not in f10 else '2011'))
            continue
        # donors = 2011 moves with motion and slot bits. Preference:
        #  1. same 2011 MVMT bank as this move's motion (= same category) and equal 2010 bits
        #  2. any bank, equal 2010 bits
        #  3. same bank, 2010 bits <= 3 apart
        #  4. same bank, the 7 nearest move ids (2010 bits empty or unmatched)
        # majority vote of the donors' 2011 bits [heuristic]
        mv = set(map_bank(p, ctx.idx10, ctx.idx11, None) for p in plan[mid]['banks10'] if p.startswith('MVMT'))
        same_bank = set(k[0] for p in mv if p for k in ctx.idx11[p][1])

        def ham(a, b):
            return bin(int.from_bytes(a, 'little') ^ int.from_bytes(b, 'little')).count('1')
        pool = [k for k in f10 if k in f11 and k in ctx.ids11 and k not in plan and f11[k][:15].strip(b'\0')]
        local = [k for k in pool if k in same_bank]
        empty = not f10[mid][:15].strip(b'\0')
        steps = [] if empty else [
            ('same bank %s, equal 2010 bits' % sorted(mv), [k for k in local if f10[k] == f10[mid]]),
            ('any bank, equal 2010 bits', [k for k in pool if f10[k] == f10[mid]])]
        if local and not empty:
            d = min(ham(f10[k], f10[mid]) for k in local)
            if d <= 3:
                steps.append(('same bank, 2010 bits %d apart' % d, [k for k in local if ham(f10[k], f10[mid]) == d]))
        if local:
            steps.append(('same bank, nearest ids', sorted(local, key=lambda k: abs(k - mid))[:7]))
        steps.append(('nearest 2010 bits anywhere', [min(pool, key=lambda k: (ham(f10[k], f10[mid]), k))]))
        for how, donors in steps:
            if donors:
                c = collections.Counter(f11[k] for k in donors)
                new = c.most_common(1)[0][0]
                how = '%s: %d donors (%s)' % (how, len(donors), ', '.join('%s x%d' % (h.hex(), n) for h, n in c.most_common(3)))
                break
        extra = ctx.extra_bits.get(mid, ())
        if extra:
            v = int.from_bytes(new, 'little')
            for bit in extra:
                v |= 1 << bit
            new = v.to_bytes(16, 'little')
            how += '; + bits %s (--extra-bits: slot requirement)' % list(extra)
        res[mid] = (f11[mid], new, how)
        ctx.p('WAZE %5d %-26s 2010 %s | 2011 now %s -> %s (%s)' % (mid, w11[i11[mid][0] + 16:i11[mid][0] + 80].split(b'\0')[0].decode('utf-8', 'replace'),
                                                              f10[mid].hex(), f11[mid].hex(), new.hex(), how))
    return res, (st11, i11)


def build(ctx, ids, namelist=False):
    out = ctx.out
    plan = analyze(ctx, ids)
    T10, T11 = ctx.T10(), ctx.T11()
    rr = ctx.rr_choice()
    # ---------------- motions
    per_bank = collections.defaultdict(list)
    for mid, it in plan.items():
        for p10, ks in it['banks10'].items():
            p11 = map_bank(p10, ctx.idx10, ctx.idx11, rr)
            if p11 is None:
                continue
            for k in ks:
                if k in ctx.idx11[p11][1]:
                    continue
                raw10 = ctx.bank(10, p10)
                e = next(e for e in bank_entries(raw10) if (e['id'], e['x'], e['y']) == k)
                if family(p10) == 'MOTP':
                    conv = e['data']   # packed MOTP stream: copied verbatim (its records are not BM-style; see re_moveport.md)
                else:
                    conv, n = convert_motion(e['data'], raw10[0x10:0x110])   # source bank's event-size table
                per_bank[p11].append(dict(y=k[2], x=k[1], id=k[0], frames=e['frames'], data=conv, src=p10))
    # sitting-in-the-corner moves: re-rooted to 2011's sitting pose (corner_shift)
    w10raw = unpack(entry_get(ctx.mg10, b'MOVS', b'WAZE'))
    _, _, wi10 = waze_load(w10raw)
    for mid in sorted(plan):
        if mid not in wi10 or w10raw[wi10[mid][0] + 0x73] != SIT_SITUATION:
            continue
        mine = [m for p11, lst in per_bank.items() if not p11.startswith('MOTP') for m in lst if m['id'] == mid]
        for n in corner_shift(mine):
            ctx.p('corner %d (sitting): %s' % (mid, n))
    # victims ending on the mat: the down motion that follows (down_end_fix)
    stock = StockEnds(ctx.pac11, os.path.join(ctx.out, 'cache', 'ends11.pkl'))
    _, _, gm = ctx.waza10()
    for mid in sorted(plan):
        exh = {}
        for g10, r in plan[mid]['exh']:
            if gm[10].get(g10) is not None:
                exh.setdefault(r[2], (gm[10][g10], r))
        mine = [dict(m, motp=p11.startswith('MOTP'), _ref=m) for p11, lst in per_bank.items() for m in lst if m['id'] == mid]
        for n in down_end_fix(mine, exh, stock, ctx.T11()):
            ctx.p('victim end %d: %s' % (mid, n))
        for n in missing_end_fix(mine, stock.ids | set(plan), ctx.T11()):
            ctx.p('missing follow-up %d: %s' % (mid, n))
        for m in mine:
            m['_ref']['data'] = m['data']
    os.makedirs(os.path.join(out, 'banks'), exist_ok=True)
    pack = dict(format='svr2011-movepack/1', moves=sorted(plan), banks=[], misc={})
    mdir = os.path.join(out, 'movepack', 'motions')
    os.makedirs(mdir, exist_ok=True)
    changed_entries = collections.defaultdict(dict)   # (typ,name) -> {child path ids: new raw}
    for p11, new in sorted(per_bank.items()):
        raw = ctx.bank(11, p11)
        nraw, added = bank_insert(raw, new)
        cap = RR_CAP if family(p11) == 'MOT/RR' else None
        ctx.p('bank %-14s %d -> %d bytes (+%d), +%d motions%s' % (p11, len(raw), len(nraw), len(nraw) - len(raw), len(added),
                                                                 '  (cap %d: %s)' % (cap, 'ok' if len(nraw) <= cap else 'OVER') if cap else ''))
        if cap and len(nraw) > cap:
            raise SystemExit('RR bank over capacity')
        open(os.path.join(out, 'banks', p11.replace('/', '_') + '.ymks'), 'wb').write(nraw)
        t, n, cids = path_split(p11)
        changed_entries[(t, n)][tuple(cids)] = nraw
        ents = []
        for m in new:
            fn = ('motp_' if p11.startswith('MOTP') else '') + '%d_%d_%d.ymk' % (m['id'], m['x'], m['y'])
            open(os.path.join(mdir, fn), 'wb').write(m['data'])
            ents.append(dict(id=m['id'], x=m['x'], y=m['y'], frames=m['frames'], file='motions/' + fn,
                             md5=hashlib.md5(m['data']).hexdigest(), from2010=m['src']))
        pack['banks'].append(dict(path=p11, pac='mpsp.pac' if p11.startswith('MOTP') else 'm.pac', kind='YMKs', size_before=len(raw), size_after=len(nraw),
                                  md5_before=hashlib.md5(raw).hexdigest(), md5_after=hashlib.md5(nraw).hexdigest(), insert=ents))
    for pacname, (hh, gg, tt) in (('m.pac', (ctx.h11, ctx.g11, ctx.tr11)), ('mpsp.pac', (ctx.ph11, ctx.pg11, ctx.ptr11))):
        groups = [(t, list(e)) for t, e in gg]
        for (t, n), subs in changed_entries.items():
            if (t == b'MOTP') != (pacname == 'mpsp.pac'):
                continue
            blob = entry_get(groups, t, n)
            for cids, nraw in subs.items():
                blob = tree_set(blob, list(cids), nraw)
            entry_set(groups, t, n, blob)
        mp = svrfmt.epac_write(hh, groups, tt)
        open(os.path.join(out, pacname), 'wb').write(mp)
        ctx.p('wrote', pacname, len(mp))
    # ---------------- misc.pac
    w10, w11, gm = ctx.waza10()
    mgroups = [(t, list(e)) for t, e in ctx.mg11]
    # WAZE category bits
    flags, (st11, i11) = waze_flags(ctx, plan, set(ids))
    wz = bytearray(unpack(entry_get(mgroups, b'MOVS', b'WAZE')))
    pack['misc']['waze'] = []
    for mid, (old, new, how) in flags.items():
        if 'waze' in os.environ.get('PORT_SKIP', '').split(','): break
        for o in i11[mid]:
            wz[o:o + 16] = new
        pack['misc']['waze'].append(dict(id=mid, flags_before=old.hex(), flags_after=new.hex(), how=how))
    waze_recount(wz)
    entry_set(mgroups, b'MOVS', b'WAZE', bytes(wz))
    # WAZA/DATA children 0 (EXH), 1 (events), 2 (MBD)
    sub0 = {g: (pk, u) for g, pk, u in w11[10]}
    sub1 = {g: (pk, u) for g, pk, u in w11[11]}
    add0 = collections.defaultdict(list)
    add1 = collections.defaultdict(list)
    for mid, it in plan.items():
        for g10, r in it['exh']:
            g11 = gm[10].get(g10)
            if g11 is None:
                ctx.p('EXH %d: 2010 group %d has no 2011 counterpart' % (mid, g10))
                continue
            add0[g11].append(r)
        for g10, r, evs in it['evt']:
            g11 = gm[11].get(g10)
            if g11 is None:
                ctx.p('EVT %d: 2010 group %d has no 2011 counterpart' % (mid, g10))
                continue
            add1[g11].append((r, evs))
    skip = os.environ.get('PORT_SKIP', '').split(',')  # (test aid: exh, evt, waze, mbd)
    if 'exh' in skip: add0.clear()
    if 'evt' in skip: add1.clear()
    pack['misc']['exh'] = []
    pack['misc']['events'] = []
    new_sub0 = {}
    for g, rs in add0.items():
        pk, u = sub0[g]
        recs = exh_parse(u, 36)
        have = set(rkey(r) for r in recs)
        todo = []
        for r in rs:
            if rkey(r) in have:
                continue
            # +u32 (new in 2011): from the most similar record of the 2011 group
            best = min(recs, key=lambda q: (sum(a != b for a, b in zip(q[2:32], r[2:32])), q[32:]))
            r36 = r + best[32:36]
            todo.append(r36)
            pack['misc']['exh'].append(dict(group=g, record=r36.hex(), extra_from='%d,%d,%d' % rkey(best)))
        if not is_sorted(recs, idkey):
            ctx.p('EXH group %d is not in id order; appending' % g)
            recs += todo
        else:
            sorted_insert(recs, todo, idkey)
        new_sub0[g] = exh_build(recs)
        ctx.p('EXH group %d: +%d records' % (g, len(todo)))
    new_sub1 = {}
    for g, rs in add1.items():
        pk, u = sub1[g]
        recs, ev = evt_parse(u)
        have = set(rkey(r) for r in recs)
        ev = bytearray(ev)
        todo = []
        for r, evs in rs:
            if rkey(r) in have:
                continue
            idx = len(ev) // 4
            if idx > 0xffff or len(evs) > 255:
                raise SystemExit('event index overflow')
            nr = bytearray(r)
            struct.pack_into('<H', nr, 8, idx)
            nr[10] = len(evs)
            ev += b''.join(evs)
            todo.append(bytes(nr))
            pack['misc']['events'].append(dict(group=g, record=bytes(nr).hex(), events=b''.join(evs).hex()))
        if not is_sorted(recs, idkey):
            ctx.p('event group %d is not in id order; appending' % g)
            recs += todo
        else:
            sorted_insert(recs, todo, idkey)
        new_sub1[g] = struct.pack('<II', 16, len(recs)) + b''.join(recs) + bytes(ev)
        ctx.p('event group %d: +%d records' % (g, len(todo)))
    # child 2 "MBD" (8-byte {u16 id, u8 x, u8 y, u16 value, u16 0} per key; 2010 and 2011
    # group indices match). 2011 kept these for every Jeff strike already.
    sub2 = {g: (pk, u) for g, pk, u in w11[12]}
    new_sub2 = {}
    pack['misc']['mbd'] = []
    for g in sorted(set(g for it in plan.values() for g, r in it['mbd'])):
        pk, u = sub2[g]
        n = struct.unpack_from('<I', u, 4)[0]
        recs = [u[8 + 8 * k:16 + 8 * k] for k in range(n)]
        have = set(rkey(r) for r in recs)
        todo = [r for it in plan.values() for gg, r in it['mbd'] if gg == g and rkey(r) not in have]
        ctx.p('MBD group %d: %d records for these moves, %d already in 2011, +%d' % (
            g, sum(1 for it in plan.values() for gg, r in it['mbd'] if gg == g), sum(1 for it in plan.values() for gg, r in it['mbd'] if gg == g and rkey(r) in have), len(todo)))
        if todo:
            sorted_insert(recs, todo, idkey)
            new_sub2[g] = b'MBD\0' + struct.pack('<I', len(recs)) + b''.join(recs)
            pack['misc']['mbd'] += [dict(group=g, record=r.hex()) for r in todo]

    def rebuild_child(childblob, newsubs):
        raw = unpack(childblob)
        ch = []
        for i, d in svrfmt.pach_read(raw):
            if i in newsubs:
                nd = repack(newsubs[i], d[:4] == b'BPE ')
                ch.append((i, nd))
            else:
                ch.append((i, d))
        return repack(svrfmt.pach_write(ch), childblob[:4] == b'BPE ')
    wraw = unpack(entry_get(mgroups, b'WAZA', b'DATA'))
    wch = dict(svrfmt.pach_read(wraw))
    c0 = rebuild_child(wch[0], new_sub0) if new_sub0 else wch[0]
    c1 = rebuild_child(wch[1], new_sub1) if new_sub1 else wch[1]
    c2 = rebuild_child(wch[2], new_sub2) if new_sub2 else wch[2]
    neww = [(i, {0: c0, 1: c1, 2: c2}.get(i, d)) for i, d in svrfmt.pach_read(wraw)]
    entry_set(mgroups, b'WAZA', b'DATA', repack(svrfmt.pach_write(neww), entry_get(ctx.mg11, b'WAZA', b'DATA')[:4] == b'BPE '))
    for nm in (b'BATS', b'BATH'):
        braw = unpack(entry_get(mgroups, nm, b'INIT'))
        bch = svrfmt.pach_read(braw)
        nb = []
        for i, d in bch:
            if i == 10:
                if unpack(d) != unpack(wch[0]):
                    raise SystemExit('%s/INIT child 10 differs from WAZA/DATA 0' % nm)
                nb.append((i, c0))
            elif i == 11:
                if unpack(d) != unpack(wch[1]):
                    raise SystemExit('%s/INIT child 11 differs from WAZA/DATA 1' % nm)
                nb.append((i, c1))
            elif i == 12:
                if unpack(d) != unpack(wch[2]):
                    raise SystemExit('%s/INIT child 12 differs from WAZA/DATA 2' % nm)
                nb.append((i, c2))
            else:
                nb.append((i, d))
        if namelist and nm in (b'BATS', b'BATH'):
            nb = patch_namelist(ctx, plan, nb, pack)
        entry_set(mgroups, nm, b'INIT', repack(svrfmt.pach_write(nb), entry_get(ctx.mg11, nm, b'INIT')[:4] == b'BPE '))
    ms = svrfmt.epac_write(ctx.mh11, mgroups, ctx.mtr11)
    open(os.path.join(out, 'misc.pac'), 'wb').write(ms)
    ctx.p('wrote misc.pac', len(ms))
    # the game finds pac entries through plist360.arc, not the pac's own table
    game = os.path.dirname(os.path.abspath(ctx.pac11))
    for arcname in ('plist360.arc', 'plist360_4x3.arc'):
        if os.path.exists(os.path.join(game, arcname)):
            refresh_arc(ctx, game, arcname, {'pac\\m.pac': os.path.join(out, 'm.pac'), 'pac\\misc.pac': os.path.join(out, 'misc.pac'),
                                             'pac\\mpsp.pac': os.path.join(out, 'mpsp.pac')}, os.path.join(out, arcname))
    pack['misc']['note'] = ('WAZA/DATA children 0/1 and BATS/INIT + BATH/INIT children 10/11 get the same records; '
                            'EXH group lists stay sorted by (id,x,b3), event records by (id,x,y) with their events appended')
    json.dump(pack, open(os.path.join(out, 'movepack', 'manifest.json'), 'w'), indent=1)
    open(os.path.join(out, 'report.txt'), 'w', encoding='utf-8').write('\n'.join(ctx.log) + '\n')
    return plan


def pac_toc(path):
    with open(path, 'rb') as f:
        d = f.read(0x4000)
    out = []
    p = 0x800
    while p + 12 <= 0x4000 and d[p:p + 4] != b'\0\0\0\0':
        typ = d[p:p + 4]
        cnt = struct.unpack_from('<I', d, p + 4)[0] // 3
        p += 12
        ents = []
        for _ in range(cnt):
            ents.append((d[p:p + 4],) + struct.unpack_from('<II', d, p + 4))
            p += 12
        out.append((typ, ents))
    return out


def refresh_arc(ctx, game, arcname, pacs, dest):
    """Copy <game>/<arcname> to dest with the sector/size of every entry of the given pacs
    refreshed (same logic as port/tools/plist_fix.py: per pac listed in pac/plist360.h, line n = pac
    index n, the arc holds a big-endian copy of its TOC after an "FF xx <u16 BE index>" record)."""
    arc = bytearray(open(os.path.join(game, arcname), 'rb').read())
    names = open(os.path.join(game, 'pac', 'plist360.h'), 'rb').read().decode('latin1').splitlines()
    starts = [(p, struct.unpack_from('>H', arc, p + 2)[0]) for p in range(0, len(arc) - 3, 4) if arc[p] == 0xFF]
    recs = {idx: (p + 4, starts[k + 1][0] if k + 1 < len(starts) else len(arc)) for k, (p, idx) in enumerate(starts)}
    lower = {k.lower(): v for k, v in pacs.items()}
    for idx, line in enumerate(names):
        line = line.strip()
        if line.lower() not in lower:
            continue
        if idx not in recs:
            ctx.p('%s: %s not in the arc' % (arcname, line))
            continue
        p, end = recs[idx]
        n = 0
        for typ, ents in pac_toc(lower[line.lower()]):
            if arc[p:p + 4] != typ:
                raise SystemExit('%s: %s group %r differs from the arc' % (arcname, line, typ))
            q = p + 12
            for name, sec, sz in ents:
                if arc[q:q + 4] != name:
                    raise SystemExit('%s: %s entry %r differs from the arc' % (arcname, line, name))
                if struct.unpack_from('>II', arc, q + 4) != (sec, sz):
                    struct.pack_into('>II', arc, q + 4, sec, sz)
                    n += 1
                q += 12
            p = q
        ctx.p('%s: %s -> %d entries refreshed' % (arcname, line, n))
    open(dest, 'wb').write(bytes(arc))


def patch_namelist(ctx, plan, nb, pack):
    """optional: BATS/INIT child 20 (2010: 19) move-name list, 104-byte records sorted by id [role unknown]"""
    b10 = bats_children(ctx.mg10)
    u10 = unpack(b10[19])
    r10 = {struct.unpack_from('<H', u10, i)[0]: u10[i:i + 100] for i in range(0, len(u10), 100)}
    out = []
    for i, d in nb:
        if i != 20:
            out.append((i, d))
            continue
        u = unpack(d)
        recs = [u[k:k + 104] for k in range(0, len(u), 104)]
        have = set(struct.unpack_from('<H', r, 0)[0] for r in recs)
        # 2010 cat -> most common 2011 6-byte field for the same moves
        m = collections.defaultdict(collections.Counter)
        for r in recs:
            mid = struct.unpack_from('<H', r, 0)[0]
            if mid in r10:
                m[struct.unpack_from('<H', r10[mid], 2)[0]][r[2:8]] += 1
        add = []
        for mid in plan:
            if mid in r10 and mid not in have:
                cat = struct.unpack_from('<H', r10[mid], 2)[0]
                f = m[cat].most_common(1)[0][0] if m[cat] else struct.pack('<HHH', cat, 0xff, 0xff)
                add.append(r10[mid][:2] + f + r10[mid][4:100])
        recs = sorted(recs + add, key=lambda r: struct.unpack_from('<H', r, 0)[0])
        ctx.p('name list (BATS/INIT 20): +%d records %s' % (len(add), [struct.unpack_from('<H', r, 0)[0] for r in add]))
        pack['misc']['namelist'] = [r.hex() for r in add]
        out.append((i, repack(b''.join(recs), d[:4] == b'BPE ')))
    return out


# ------------------------------------------------------------------ verify

def verify(pac11, out):
    log = []

    def p(*a):
        s = ' '.join(str(x) for x in a)
        print(s)
        log.append(s)
    pack = json.load(open(os.path.join(out, 'movepack', 'manifest.json')))
    oh, og, otr = read_epac(os.path.join(pac11, 'm.pac'))
    nh, ng, ntr = read_epac(os.path.join(out, 'm.pac'))
    og = og + read_epac(os.path.join(pac11, 'mpsp.pac'))[1]     # MOTP/GAME lives in mpsp.pac in 2011
    ng = ng + read_epac(os.path.join(out, 'mpsp.pac'))[1]
    changed = set(tuple(b['path'].split('/')[:2]) for b in pack['banks'])
    ok = True
    # 1. EPAC: same entries, unchanged entries byte-identical
    on = [(t, n) for t, e in og for n, _ in e]
    nn = [(t, n) for t, e in ng for n, _ in e]
    if on != nn:
        p('FAIL entry list differs')
        ok = False
    same = diff = 0
    for (t, e), (t2, e2) in zip(og, ng):
        for (n, b), (n2, b2) in zip(e, e2):
            key = (t.decode().strip(), n.decode().strip())
            if b == b2:
                same += 1
            elif key in changed:
                diff += 1
            else:
                p('FAIL unexpected change', key)
                ok = False
    p('m.pac + mpsp.pac entries: %d unchanged (byte-identical), %d changed (expected %d)' % (same, diff, len(changed)))
    # 2. changed banks: structure, all motions decode, originals unchanged, new = payload
    T = None
    for b in pack['banks']:
        t, n, cids = path_split(b['path'])
        oraw = tree_get(entry_get(og, t, n), cids)
        nraw = tree_get(entry_get(ng, t, n), cids)
        T = nraw[0x10:0x110]
        packed = b['path'].startswith('MOTP')   # MOTP records are packed: only the frame framing is checked
        assert nraw[:0x110] == oraw[:0x110], 'header/T changed'
        base, ents = ydir(nraw)
        keys = [key_of(e) for e in ents]
        offs = [e[3] for e in ents]
        okeys = [key_of(e) for e in ydir(oraw)[1]]
        unsorted_before = sum(okeys[i] > okeys[i + 1] for i in range(len(okeys) - 1))
        unsorted_after = sum(keys[i] > keys[i + 1] for i in range(len(keys) - 1))
        good = unsorted_after == unsorted_before and all(offs[i] <= offs[i + 1] for i in range(len(offs) - 1)) \
            and offs[0] == 0 and offs[-1] < len(nraw) - base and ents[-1][2] in (0x7fff, 16000)
        oe = {(e['id'], e['x'], e['y'], i): e for i, e in enumerate(bank_entries(oraw))}
        ne = bank_entries(nraw)
        nmap = collections.defaultdict(list)
        for e in ne:
            nmap[(e['id'], e['x'], e['y'])].append(e)
        unchanged = 0
        for (i_, x_, y_, k_), e in oe.items():
            cands = nmap[(i_, x_, y_)]
            if any(c['data'] == e['data'] and c['frames'] == e['frames'] for c in cands):
                unchanged += 1
            else:
                # duplicate-offset entries have empty data before; compare by first non-empty
                if not e['data'] or any(c['data'].startswith(e['data']) for c in cands):
                    unchanged += 1
                else:
                    p('FAIL original motion changed', b['path'], (i_, x_, y_))
                    ok = False
        dec_err = 0
        for e in ne:
            if e['id'] in SENTINEL_IDS:
                continue
            try:
                nf, recs = frames(e['data'])
                if nf != e['frames'] and e['frames']:
                    pass
                for f, q, sz in recs:
                    r = e['data'][q:q + sz]
                    if r and r[0] & 0x80 and not packed:
                        if not events(r[1:1 + (r[0] & 0x7f)], T)[1]:
                            raise ValueError('events')
            except ValueError:
                dec_err += 1
        oerr = 0
        for e in bank_entries(oraw):
            if e['id'] in SENTINEL_IDS:
                continue
            try:
                nf, recs = frames(e['data'])
                for f, q, sz in recs:
                    r = e['data'][q:q + sz]
                    if r and r[0] & 0x80 and not packed and not events(r[1:1 + (r[0] & 0x7f)], T)[1]:
                        raise ValueError
            except ValueError:
                oerr += 1
        ins_ok = 0
        for m in b['insert']:
            d = open(os.path.join(out, 'movepack', m['file']), 'rb').read()
            c = [e for e in nmap[(m['id'], m['x'], m['y'])] if e['data'] == d and e['frames'] == m['frames']]
            ins_ok += bool(c)
        p('%-14s n %d -> %d  sorted/offsets %s  originals unchanged %d/%d  new found %d/%d  undecodable %d (stock bank: %d)' % (
            b['path'], len(oe), len(ne), 'ok' if good else 'BAD', unchanged, len(oe), ins_ok, len(b['insert']), dec_err, oerr))
        ok &= good and unchanged == len(oe) and ins_ok == len(b['insert']) and dec_err <= oerr
    # 3. misc.pac
    mh, mg, mtr = read_epac(os.path.join(pac11, 'misc.pac'))
    nh2, ng2, ntr2 = read_epac(os.path.join(out, 'misc.pac'))
    for (t, e), (t2, e2) in zip(mg, ng2):
        for (n, b), (n2, b2) in zip(e, e2):
            if b != b2:
                p('misc.pac changed entry', t.decode(), n.decode(), len(b), '->', len(b2))
    w = waza_children(ng2)
    bs = bats_children(ng2)
    bh = bats_children(ng2, b'BATH')
    for c in (0, 1, 2):
        same = unpack(w[c]) == unpack(bs[10 + c]) == unpack(bh[10 + c])
        p('WAZA/DATA %d == BATS/INIT %d == BATH/INIT %d: %s' % (c, 10 + c, 10 + c, same))
        ok &= same
    exh = {}
    for g, _, u in subgroups(w[0]):
        recs = exh_parse(u, 36)
        if u[:4] == b'EXH\0' and len(u) != 8 + 36 * len(recs):
            p('FAIL EXH group size', g)
            ok = False
        if not is_sorted(recs, idkey):
            p('FAIL EXH group %d not in move id order (the game binary-searches the id)' % g)
            ok = False
        for r in recs:
            exh.setdefault(rkey(r), r)
    evk = {}
    for g, _, u in subgroups(w[1]):
        pr = evt_parse(u)
        if not pr:
            continue
        recs, ev = pr
        for r in recs:
            i = struct.unpack_from('<H', r, 8)[0]
            if 4 * (i + r[10]) > len(ev):
                p('FAIL event index out of range', g, rkey(r))
                ok = False
            evk[rkey(r)] = evt_list(r, ev)
    for x in pack['misc']['exh']:
        r = bytes.fromhex(x['record'])
        if exh.get(rkey(r)) != r:
            p('FAIL EXH record not found', rkey(r))
            ok = False
    for x in pack['misc']['events']:
        r = bytes.fromhex(x['record'])
        if b''.join(evk.get(rkey(r), [])) != bytes.fromhex(x['events']):
            p('FAIL event record not found', rkey(r))
            ok = False
    p('EXH records added %d, event records added %d: all found' % (len(pack['misc']['exh']), len(pack['misc']['events'])))
    wz = unpack(entry_get(ng2, b'MOVS', b'WAZE'))
    st, n, ids = waze_load(wz)
    for x in pack['misc']['waze']:
        got = [wz[o:o + 16].hex() for o in ids[x['id']]]
        if any(g != x['flags_after'] for g in got):
            p('FAIL WAZE flags', x['id'])
            ok = False
    p('WAZE flags set for %d moves' % len(pack['misc']['waze']))
    p('VERIFY', 'OK' if ok else 'FAILED')
    open(os.path.join(out, 'verify.txt'), 'w').write('\n'.join(log) + '\n')
    return ok


def fix_svrmod(pac10, pac11, path, dry=False, cache=None):
    """An already built SvR 2010 superstar mod (.svrmod): its ported sitting-corner
    moves re-rooted (corner_shift) and victims ending on the mat chained to a
    down motion (down_end_fix), version +0.1. Returns the notes (empty: nothing to do)."""
    import zipfile
    h, g, t = read_epac(os.path.join(pac10, 'misc.pac'))
    w10 = unpack(entry_get(g, b'MOVS', b'WAZE'))
    _, _, wi10 = waze_load(w10)
    with zipfile.ZipFile(path) as z:
        infos = z.infolist()
        files = {i.filename: z.read(i) for i in infos}
    motions = collections.defaultdict(list)
    for n in files:
        m = re.fullmatch(r'moves/motions/(motp_)?(\d+)_(\d+)_(\d+)(_p)?\.ymk', n)   # (_p: the SvR 2008 ports' packed copies)
        if m:
            motions[int(m[2])].append(dict(id=int(m[2]), x=int(m[3]), y=int(m[4]), data=files[n], name=n,
                                           motp=bool(m[1] or m[5])))
    notes = []
    kinds = []
    # victims ending on the mat (BM and MOTP copies)
    stock = StockEnds(pac11, cache or os.path.join(HERE, 'out', 'cache', 'ends11.pkl'))
    h, g, t = read_epac(os.path.join(pac11, 'm.pac'))
    T11 = tree_get(entry_get(g, *path_split('MVMT/CORN/f')[:2]), path_split('MVMT/CORN/f')[2])[0x10:0x110]
    exh = collections.defaultdict(dict)
    for l in files.get('moves/pack.txt', b'').decode('utf-8', 'replace').splitlines():
        f = l.split()
        if len(f) >= 3 and f[0] == 'exh':
            r = bytes.fromhex(f[2])
            exh[rkey(r)[0]].setdefault(r[2], (int(f[1]), r))
    for mid in sorted(motions):
        got = down_end_fix(motions[mid], exh.get(mid, {}), stock, T11)
        for n in got:
            notes.append('victim end %d: %s' % (mid, n))
        missing = missing_end_fix(motions[mid], stock.ids | set(motions), T11)
        for n in missing:
            notes.append('missing follow-up %d: %s' % (mid, n))
        if got or missing:
            for m in motions[mid]:
                files[m['name']] = m['data']
    if notes:
        kinds.append(b'# victims ending on the mat chained to a down motion (svr10_moves.py fix)')
    corner = len(notes)
    for mid in sorted(motions):
        if mid not in wi10 or w10[wi10[mid][0] + 0x73] != SIT_SITUATION:
            continue
        bm = [m for m in motions[mid] if not m['motp']]   # (motp_*: packed poses, not shifted)
        for n in corner_shift(bm):
            notes.append('corner %d (sitting): %s' % (mid, n))
        for m in bm:
            files[m['name']] = m['data']
    if len(notes) > corner:
        kinds.append(b'# sitting-corner moves re-rooted (svr10_moves.py corner)')
    if not notes or dry:
        return notes
    man = files['manifest.txt'].decode('utf-8').split('\n')
    for i, l in enumerate(man):
        if l.startswith('version='):
            try:
                man[i] = 'version=%.1f' % (float(l[8:].strip()) + 0.1) + ('\r' if l.endswith('\r') else '')
            except ValueError:
                pass
    files['manifest.txt'] = '\n'.join(man).encode('utf-8')
    # (pack.txt changes too: the game rebuilds its move overlay only when a pack.txt does)
    if 'moves/pack.txt' in files:
        pk = files['moves/pack.txt']
        eol = b'\r\n' if b'\r\n' in pk else b'\n'
        files['moves/pack.txt'] = pk.rstrip(b'\r\n') + eol + b''.join(k + eol for k in kinds if k not in pk)
    tmp = path + '.tmp'
    with zipfile.ZipFile(tmp, 'w') as z:
        for i in infos:
            z.writestr(i, files[i.filename], compress_type=i.compress_type)
    os.replace(tmp, path)
    return notes


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    out = os.path.join(HERE, 'out')
    if '--out' in argv:
        i = argv.index('--out')
        out = argv[i + 1]
        del argv[i:i + 2]
    namelist = '--namelist' in argv
    extra = {}
    if '--extra-bits' in argv:
        i = argv.index('--extra-bits')
        for line in open(argv[i + 1]):
            line = line.split('#')[0].split()
            if len(line) >= 2:
                extra[int(line[0])] = [int(b) for b in line[1].split(',') if b]
        del argv[i:i + 2]
    argv = [a for a in argv if a != '--namelist']
    cmd = argv[1]
    if cmd == 'fix':   # fix <pac10> <pac11> <mod.svrmod>... [--dry]
        for f in argv[4:]:
            if f != '--dry':
                for n in fix_svrmod(argv[2], argv[3], f, '--dry' in argv, os.path.join(out, 'cache', 'ends11.pkl')):
                    print('%s: %s' % (os.path.basename(f), n))
        return 0
    if cmd == 'verify':
        return 0 if verify(argv[2], out) else 2
    pac10, pac11, ids = argv[2], argv[3], parse_ids(argv[4])
    os.makedirs(out, exist_ok=True)
    ctx = Ctx(pac10, pac11, out)
    ctx.extra_bits = extra
    if cmd == 'analyze':
        plan = analyze(ctx, ids)
        waze_flags(ctx, plan, set(ids))
        open(os.path.join(out, 'analysis.txt'), 'w', encoding='utf-8').write('\n'.join(ctx.log) + '\n')
    elif cmd == 'build':
        build(ctx, ids, namelist)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
