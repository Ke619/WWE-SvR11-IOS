"""PS2 superstars' movesets -> SvR 2011 slots, for tools/ps2_port.py (moveset_port.port_moveset, no porter).

    moveset_step(g, cid, base, o, work, mod, man, rep)    # ps2_port.build_ps2's moves step
    py ps2_moves.py --write-maps                         # the ps2_* slot maps into moveset_maps.json
    py ps2_moves.py --report <game> <iso> [--pac11 <2011 pac>]   # every superstar's slots: mapped / kept

A slot's move goes into the mapped 2011 slot when 2011 has the same move (by name; the spelling evened out:
"Bulldog 08" = "Bulldog 8", "The Rock Bottom" = "Rock Bottom") with a motion, in that slot or its menu group
(moveset_port's rules); else the base's move stays (PS2 motions aren't ported). Where (research 2026-10-14):

  game       move names (id: name)                                      profile (u16 move ids)
  ps2_jbi    EDIT.PAC group MOVS (a PAC), entry 999 (BPE): lists of     CHAR record (512 B) +0x0C..+0xE4 (SYM's + 4;
             ~39-byte {.., u16 id, name} (the id right before the name)  SYM's move ids too)
  ps2_sym    PAC/MOVES.PAC CATE: a PAC of lists, u32 n + 40-byte        CHAR record (ps2_records) +0x08..+0xF2
             {u16 id, ., name +8}
  ps2_hctp   PAC/MOVES.PAC WAZA: 32-byte header, 64-byte {name[32],     CHAR record +0x10..+0x110
             data[32]: id +8}
  ps2_svr04  PAC/MOVES.PAC WAZA: 0xA0-byte header, then as HCTP         CHAR record +0x30..+0x130 (HCTP's + 0x20)
  ps2_svr06  PAC/MISC.PAC MOVS/WAZE: 144-byte {name +0x10, id +0x88}    PAC/M.PAC CHAR/PRO (432 B) +0x00..+0x12E
             (SvR 2007's)
  ps2_svr07..09  as the X360 games byte for byte: their maps (moveset_maps.json svr2007-2009), 2011's ids
  ps2_svr10  MOVS/WAZE 160-byte records (svr10_superstar's)            CHAR/PRO: the X360 record's first 564 B;
                                                                        svr10_superstar.MAP for the slots
The MOVES.PAC (JBI: EDIT.PAC) directory is one level (0x800: tag, u32 n, {tag, u16 sector, u16 size / 0x100}
until a zero tag - JBI's lists more than n - data at 0x4000 + sector * 0x800); MISC.PAC / M.PAC groups as
ps2_records._groups.
SYM / HCTP / SvR 2004 share one layout (SYM 8 bytes before HCTP, 2004 0x20 after, SYM ends at HCTP's +0xFA);
the maps go section by section (taunts, finishers, strikes, front / rear grapples, ground, corner, top rope,
running, Irish whip, tag team) in the slots' order - PS2 menus aren't 2011's, so a slot lands on a 2011 slot of
the same kind, not always the same button.
"""
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# the X360 twin whose map / ids the PS2 2007-2009 discs use (their profiles and move ids are the X360 games')
TWIN_MAP = {'ps2_svr07': 'svr2007', 'ps2_svr08': 'svr2008', 'ps2_svr09': 'svr2009'}
SAME_IDS = ('ps2_svr07', 'ps2_svr08', 'ps2_svr09', 'ps2_svr10')
NAMES = {   # game: (file, group or None = MOVES.PAC's own directory, entry, format)
    'ps2_jbi': ('EDIT.PAC', None, 'MOVS', 'jbi'),
    'ps2_sym': ('PAC/MOVES.PAC', None, 'CATE', 'cate'), 'ps2_hctp': ('PAC/MOVES.PAC', None, 'WAZA', 'wz32'),
    'ps2_svr04': ('PAC/MOVES.PAC', None, 'WAZA', 'wzA0'), 'ps2_svr06': ('PAC/MISC.PAC', b'MOVS', 'WAZE', 'w144'),
    'ps2_svr07': ('PAC/MISC.PAC', b'MOVS', 'WAZE', 'w144'), 'ps2_svr08': ('PAC/MISC.PAC', b'MOVS', 'WAZE', 'w144'),
    'ps2_svr09': ('PAC/MISC.PAC', b'MOVS', 'WAZE', 'w144'), 'ps2_svr10': ('PAC/MISC.PAC', b'MOVS', 'WAZE', 'w160'),
}
PRO_GAMES = ('ps2_svr06', 'ps2_svr07', 'ps2_svr08', 'ps2_svr09', 'ps2_svr10')   # profile = CHAR/PRO, else the DAT record


def _r(a, n):
    return [a + 2 * k for k in range(n)]


# 2011 slots (tools/pro_slots.json)
T11 = _r(0x12, 4)
FIN11 = [0x22, 0x24]
STRIKES11, STRONG11 = _r(0x4E, 10), [0x62, 0x64]
GA, GB, GC, GD, GE = _r(0x7A, 4), _r(0x8E, 4), _r(0x98, 4), _r(0xA2, 4), _r(0xAC, 4)
GSTRIKE11 = [0x8C, 0x96, 0xA0, 0xAA]
REAR11 = _r(0xBC, 4)
# HCTP's slots (record offsets) -> 2011's
HCTP = [(_r(0x12, 4), T11), ([0x20, 0x22], FIN11),
        (_r(0x30, 4), GA), (_r(0x38, 4), GC), (_r(0x102, 4), GD), (_r(0x10A, 4), GE), ([0xFC, 0xFE, 0x100], GSTRIKE11[:3]),
        (_r(0x40, 7), STRIKES11[:7]), ([0xF2, 0xF4], STRIKES11[7:9]),
        (_r(0x5A, 4), REAR11),
        ([0x66, 0x6A, 0x68, 0x70, 0x74, 0x72, 0x82], [0xCC, 0xCE, 0xD0, 0xD8, 0xDA, 0xDC, 0xE8]),   # ground
        ([0x8A, 0x8C, 0x8E], [0x106, 0x10A, 0x10C]), ([0x94, 0x96], [0x11E, 0x122]), ([0x9C], [0x12E]),
        ([0xA2, 0xA4], [0x124, 0x126]),   # corner: front, rear, running, seated
        ([0xA6, 0xA8, 0xAA, 0xAE], _r(0x150, 4)), ([0xB6], [0x158]), ([0xB0], [0x1A6]),   # top rope, springboard, outside
        ([0xB8, 0xBA], [0x172, 0x174]), ([0xB2, 0xC0], [0x176, 0x178]), ([0xB4, 0xC4], [0x17A, 0x17C]),   # running
        (_r(0xCA, 4), _r(0x17E, 4)), (_r(0xD4, 5), _r(0x18E, 5))]   # Irish whip, tag team


def _shift(sections, d, upto=None):
    """the sections' source offsets + d (only those up to `upto`, in the unshifted offsets)"""
    out = []
    for ss, tt in sections:
        k = [s for s in ss if upto is None or s <= upto]
        if k:
            out.append(([s + d for s in k], tt[:len(k)]))
    return out


SVR04 = _shift(HCTP, 0x20)
# SYM: HCTP's minus 8 up to HCTP's +0xFA; its +0x1E..+0x24 (HCTP's grapple stances) are front grapples too
SYM = ([(_r(0x1E, 4), GA), (_r(0x28, 4), GC), (_r(0x30, 4), GD)]
       + [(ss, tt) for ss, tt in _shift(HCTP, -8, upto=0xFA) if tt not in (GA, GC, GD, GE)])
SVR06 = [(_r(0x0A, 4), T11), ([0x18, 0x1A], FIN11),
         (_r(0x1C, 9), STRIKES11[:9]),
         (_r(0x40, 4), GA), (_r(0x5E, 4), GC), (_r(0x66, 4), GD), (_r(0x6E, 4), GE), (_r(0x76, 3), GB[:3]),
         ([0x5A], [0x82]), ([0x7C, 0x7E, 0x80], GSTRIKE11[:3]),
         (_r(0x96, 4), REAR11),
         ([0xA2, 0xA6, 0xA4, 0xAC, 0xB0, 0xAE, 0xB4], [0xCC, 0xCE, 0xD0, 0xD8, 0xDA, 0xDC, 0xE8]),
         ([0xBE, 0xC0, 0xC2], [0x106, 0x10A, 0x10C]), ([0xCC, 0xCE], [0x11E, 0x122]), ([0xD6], [0x12E]),
         ([0xDC, 0xDE], [0x124, 0x126]),
         ([0xE0, 0xE2, 0xE4, 0xE8], _r(0x150, 4)), ([0xF0], [0x158]), ([0xEA], [0x1A6]),
         ([0xF2, 0xF4], [0x172, 0x174]), ([0xEC, 0xF6], [0x176, 0x178]), ([0xEE, 0xFA], [0x17A, 0x17C]),
         (_r(0xFE, 4), _r(0x17E, 4)), (_r(0x108, 5), _r(0x18E, 5))]
JBI = _shift(SYM, 4)   # (JBI's record: SYM's slots 4 bytes on, the same sections)
SECTIONS = {'ps2_jbi': JBI, 'ps2_sym': SYM, 'ps2_hctp': HCTP, 'ps2_svr04': SVR04, 'ps2_svr06': SVR06}


def slot_map(game):
    """{source byte offset: 2011 byte offset}"""
    if game == 'ps2_svr10':
        import svr10_superstar as SS
        return {s10: s11 for s10, s11 in SS.MAP if s10 < 0x174}
    m = {}
    for ss, tt in SECTIONS[game]:
        assert len(ss) == len(tt), (game, ss, tt)
        for s, t in zip(ss, tt):
            m.setdefault(s, t)
    return m


def map_key(game):
    """the moveset_maps.json entry the game's slots use"""
    return TWIN_MAP.get(game, game)


# --------------------------------------------------------------------------- reading the disc

def _moves_dir(head, entry):
    for k in range(64):   # (until a zero tag: JBI's EDIT.PAC lists more groups than its count says)
        p = 0x808 + 8 * k
        if head[p:p + 4] == bytes(4):
            break
        if head[p:p + 4].decode('latin-1').strip() == entry:
            sec, size = struct.unpack_from('<HH', head, p + 4)
            return 0x4000 + sec * 0x800, size * 0x100
    return None


def _entry(d, f, group, entry):
    head = d.read(f, 0, 0x4000)
    if group is None:
        at = _moves_dir(head, entry)
    else:
        import ps2_records as PR
        at = next(((o, s) for t, n, o, s in PR._groups(head) if t == group and n.strip() == entry), None)
    if at is None:
        raise ValueError('%s has no %s' % (f, entry))
    return bytes(d.read(f, at[0], at[1]))


def _names(b, fmt):
    out = {}
    if fmt in ('wz32', 'wzA0'):   # {name[32], data[32]: id +8} after the header
        for o in range(0x20 if fmt == 'wz32' else 0xA0, len(b) - 63, 64):
            out.setdefault(struct.unpack_from('<H', b, o + 40)[0], b[o:o + 32].split(b'\0')[0].decode('latin-1').strip())
    elif fmt == 'jbi':   # a PAC; entry 999 (BPE): the move lists, each name right after its u16 id
        import ps2_char
        import svrfmt
        v = ps2_char.pac(b)[999]
        v = svrfmt.bpe_decode(v) if v[:4] == b'BPE ' else v
        for m in re.finditer(rb'[A-Za-z0-9][ -~]{2,}?\x00', v):
            if m.start() >= 2:
                i = struct.unpack_from('<H', v, m.start() - 2)[0]
                if 100 <= i < 40000:
                    out.setdefault(i, m.group()[:-1].decode('latin-1').strip())
    elif fmt == 'cate':   # a PAC of category lists: u32 n, n x 40 bytes {u16 id, ., name +8}
        import ps2_char
        import svrfmt
        for v in ps2_char.pac(b).values():
            v = svrfmt.bpe_decode(v) if v[:4] == b'BPE ' else v
            for i in range(struct.unpack_from('<I', v, 0)[0] if len(v) >= 4 else 0):
                o = 4 + 40 * i
                if o + 40 > len(v):
                    break
                out.setdefault(struct.unpack_from('<H', v, o)[0], v[o + 8:o + 40].split(b'\0')[0].decode('latin-1').strip())
    elif fmt == 'w144':   # SvR 2007's (moveset_maps.waze_old)
        for p in range(0, len(b) - 143, 144):
            nm = b[p + 0x10:p + 0x50].split(b'\0')[0]
            if len(nm) > 2 and all(32 <= c < 127 for c in nm):
                out.setdefault(struct.unpack_from('<H', b, p + 0x88)[0], nm.decode('latin-1').strip())
    elif fmt == 'w160':   # SvR 2010's (svr10_superstar Game.waze)
        st = b.index(b'* test motion *') - 0x10
        for k in range((len(b) - st) // 160):
            r = b[st + 160 * k:st + 160 * (k + 1)]
            if len(r) == 160 and r.strip(b'\0'):
                out.setdefault(struct.unpack_from('<H', r, 0x90)[0], r[0x10:0x50].split(b'\0')[0].decode('utf-8', 'replace'))
    return {k: v for k, v in out.items() if v}


def names(src):
    """{move id: name} of the game (cached on src)"""
    if '_ps2_moves' not in src.__dict__:
        f, group, entry, fmt = NAMES[src.game_id]
        src._ps2_moves = _names(_entry(src.g.disc, f, group, entry), fmt)
    return src._ps2_moves


def profile(src, cid):
    """the superstar's move profile (bytes, the map's offsets) or None"""
    import ps2_records as PR
    n = PR.number(src, cid)
    if n is None:
        return None
    return PR.table(src, 'PRO').get(n) if src.game_id in PRO_GAMES else PR.table(src)[n]


def _spell(s):
    s = re.sub(r'\b0+(\d)', r'\1', s.lower().strip())
    if s.startswith('taunt '):   # (HCTP: "Taunt The Rock 1" = 2011's "The Rock 1", "Taunt Austin" = "Stone Cold")
        s = re.sub(r'^(steve )?austin\b', 'stone cold', s[6:])
    return re.sub(r'^the ', '', s)


def names_2011(waze, g11):
    """the PS2 names, 2011's spelling where only the spelling differs ("Bulldog 08" -> "Bulldog 8")"""
    import moves_port as MP
    by = {}
    for i, (bits, nm) in g11.waze().items():
        by.setdefault(MP.norm(_spell(nm)), nm)
    have = {MP.norm(nm) for bits, nm in g11.waze().values()}
    out = {}
    for i, nm in waze.items():
        out[i] = nm if MP.norm(nm) in have else by.get(MP.norm(_spell(nm)), nm)
    return out


# --------------------------------------------------------------------------- the port step

def _stem(s):
    import moves_port as MP
    return MP.norm(re.sub(r'\s*\d+$', '', _spell(s)))


_G11 = {}


def _g11_slots(g11, pac11, work):
    """(moves with a motion, {move: slots 2011's roster has it in}, {move: menu groups}, {slot: menu}, {stem: ids})"""
    if pac11 not in _G11:
        import collections
        import moveset_port
        slots = {int(s['off']): s for s in json.load(open(os.path.join(HERE, 'pro_slots.json')))['slots']}
        menu_of = {off: (sl.get('menu') or '') for off, sl in slots.items()}
        in_slot, in_menu = collections.defaultdict(collections.Counter), collections.defaultdict(set)
        for r in g11.pro.values():
            if len(r) >= 2 * moveset_port.N11:
                for k, x in enumerate(struct.unpack_from('<%dH' % moveset_port.N11, r, 0)):
                    in_slot[x][2 * k] += 1
                    in_menu[x].add(menu_of.get(2 * k, ''))
        stems = collections.defaultdict(list)
        for i, (bits, nm) in g11.waze().items():
            stems[_stem(nm)].append(i)
        _G11[pac11] = (moveset_port.g11_moves(pac11, work).moved, in_slot, in_menu, menu_of, stems)
    return _G11[pac11]


def similar(res, g11, base, pac11, work, near=True):
    """the slots port_moveset kept: a 2011 move of the same name with another number ("Snap Jab" ->
    "Snap Jab 2") that 2011's roster uses in that slot or its menu group -> 'similar move in 2011 (name)'
    (near=False: only the same id and name -> 'same move in 2011 (same id: name)')"""
    import moves_port as MP
    moved, in_slot, in_menu, menu_of, stems = _g11_slots(g11, pac11, work)
    w11, w = g11.waze(), res['ps2_names']
    area = list(struct.unpack_from('<%dH' % (len(g11.pro[base]) // 2), g11.pro[base], 0))
    for ln in res['moves_txt']:
        k, v = ln.split('=')
        area[int(k, 16) // 2] = int(v)
    rows = []
    for row in res['rows']:
        so, to, v, nm, verdict, nid = row
        st = _stem(w.get(v, nm) or '')
        if verdict.startswith('base kept') and so is not None and st and not verdict.startswith('base kept (a taunt'):
            c = [x for x in stems.get(st, ()) if x in moved and MP.usable11(w11, x) and
                 (to in in_slot[x] or menu_of.get(to, '') in in_menu[x])]
            if v in c:   # (the same id and name in both games: the series kept the move - its own record)
                area[to // 2] = v
                row = (so, to, v, nm, 'same move in 2011 (same id: %s)' % w11[v][1], v)
            elif c and near:
                x = max(c, key=lambda x: (in_slot[x][to], -x))   # (the one 2011's roster has in this slot most)
                area[to // 2] = x
                row = (so, to, v, nm, 'similar move in 2011 (%s)' % w11[x][1], x)
        rows.append(row)
    res['rows'] = rows
    first = struct.unpack_from('<%dH' % (len(g11.pro[base]) // 2), g11.pro[base], 0)
    res['moves_txt'] = ['0x%03X=%d' % (k * 2, x) for k, x in enumerate(area) if x != first[k]]
    return res


def port(g, cid, base, g11, pac11, work, mod=None, rep=None, near=True):
    """moveset_port.port_moveset's result for the superstar (+ the similar moves; near=False: the same moves
    only - superstar_port --same-moves), or None (no profile / map)"""
    import moveset_port
    pro = profile(g, cid)
    if pro is None:
        return None
    w = names(g)
    c = moveset_port.Ctx(gid=map_key(g.game_id), cid=cid, base=base, g11=g11, pro=pro, waze=names_2011(w, g11),
                         mgame=None, pac11=pac11, svrmod=None, work=work, mod=mod, rep=rep, port=False,
                         max_port=0, port_taunts=False, same_ids=g.game_id in SAME_IDS)
    res = moveset_port.port_moveset(c)
    if res is not None:
        res['ps2_names'] = w
        similar(res, g11, base, pac11, work, near)
        # (slots without a named move - empty, 2010's fixed markers - keep the base's and aren't reported)
        res['rows'] = [r for r in res['rows'] if w.get(r[2]) or not r[4].startswith('base kept')]
    return res


def labelled(res, label, g11):
    """moves.txt's lines, each slot under a comment saying where its move came from (the game and the Mod Maker
    skip '#' lines; no '=' in them):  # similar: Snap Jab (Here Comes the Pain) -> Snap Jab 1"""
    import moves_port as MP
    why = {}
    for so, to, v, nm, verdict, nid in res['rows']:
        if verdict.startswith(('same move', 'similar move')):
            why[to] = ('similar' if verdict.startswith('similar') else 'same', res['ps2_names'].get(v, nm))
    w11 = g11.waze()
    out = []
    for ln in res['moves_txt']:
        off = int(ln.split('=')[0], 16)
        if off in why:
            kind, nm = why[off]
            to_nm = w11.get(int(ln.split('=')[1]), (b'', ''))[1]
            c = '# %s: %s (%s)%s' % (kind, nm, label, ' -> %s' % to_nm if to_nm and MP.norm(to_nm) != MP.norm(nm) else '')
            out.append(c.replace('=', '-'))
        out.append(ln)
    return out


def moveset_step(g, cid, base, o, work, mod, man, rep):
    """ps2_port.build_ps2's moves step (as superstar_port.moveset_step): moves.txt over the base's profile"""
    import moveset_port
    note, drop = rep['notes'].append, rep['dropped'].append
    try:
        res = port(g, cid, base, o['g11'], o['pac11'], work, mod, rep, near=not o.get('same_moves'))
    except (OSError, ValueError, KeyError, struct.error) as e:   # (a disc not as researched: the base's, said why)
        drop('moveset: the base\'s (%s moves could not be read: %s)' % (g.label, e))
        return
    if res is None:
        drop('moveset: the base\'s (no %s move profile for this superstar)' % g.label)
        return
    w = res['ps2_names']
    if res['moves_txt']:
        open(os.path.join(mod, 'moves.txt'), 'w', newline='\n').write('\n'.join(labelled(res, g.label, o['g11'])) + '\n')
        man['moves'] = 'moves.txt'
    with open(os.path.join(work, 'moves_table.tsv'), 'w', encoding='utf-8') as f:
        f.write('src_off\toff2011\tsrc_id\tname\tverdict\tid_in_slot\n')
        for so, to, v, nm, verdict, nid in res['rows']:
            f.write('%s\t0x%03x\t%d\t%s\t%s\t%d\n' % ('' if so is None else '0x%03x' % so, to, v, w.get(v, nm),
                                                      verdict, nid))
    note('moveset: %d slots over the base (%s; by name - PS2 motions aren\'t ported); table in .work/moves_table.tsv'
         % (len(res['moves_txt']), moveset_port.summary(res)))
    kept = [r for r in res['rows'] if r[4].startswith('base kept')]
    if kept:
        ex = sorted({w.get(r[2], r[3]) for r in kept if w.get(r[2], r[3])})
        drop('moveset: %d slots keep the base\'s move - no 2011 move of that name for that kind of slot: %s%s' % (
            len(kept), ', '.join(ex[:12]), ' ...' if len(ex) > 12 else ''))


# --------------------------------------------------------------------------- maps / report

def write_maps():
    import moveset_maps as MM
    data = json.load(open(MM.OUT)) if os.path.exists(MM.OUT) else {}
    for game in ('ps2_jbi', 'ps2_sym', 'ps2_hctp', 'ps2_svr04', 'ps2_svr06', 'ps2_svr10'):
        m = slot_map(game)
        why = ('svr10_superstar.MAP (the X360 2010 record\'s move block)' if game == 'ps2_svr10' else
               'laid out by section from the roster (tools/ps2_moves.py SECTIONS)')
        data[game] = {'about': '%s profile u16 slot (byte offset, hex) -> [2011 byte offset, 0]; %s' % (game, why),
                      'map': {'%03x' % s: ['%03x' % t, 0.0] for s, t in sorted(m.items())}}
    with open(MM.OUT, 'w', newline='\n') as f:
        json.dump(data, f, indent=1, sort_keys=True)
    print('ps2 maps written to', MM.OUT)


def report(game, iso, pac11, limit=None):
    """every named superstar of the disc through port(): mapped / kept counts, the moves 2011 lacks"""
    import collections
    import tempfile
    import discfs
    import ps2_records as PR
    import superstar_port as SP

    class Src:
        def __init__(self):
            self.game_id, self.g = game, type('G', (), {})()
            self.g.disc = discfs.open(iso)
            self.label = game

        def name(self, c):
            return PR.record_name(game, PR.table(self)[c]) if c in PR.table(self) else ''

        def record_number(self, c):
            return c
    src = Src()
    g11, _ = SP.game11(pac11)
    import ps2_port
    by11 = {}
    for i in SP.playable11(g11):
        by11.setdefault(ps2_port.norm(g11.name(i)), i)
    work = tempfile.mkdtemp(prefix='ps2moves_')
    tot = collections.Counter()
    missing = collections.Counter()
    rows = []
    for n in sorted(PR.table(src)):
        nm = src.name(n)
        if not nm or nm.lower().startswith(('moves set', 'edit')):
            continue
        base = by11.get(ps2_port.norm(nm)) or 104
        res = port(src, n, base, g11, pac11, work)
        if res is None:
            continue
        c = collections.Counter('same' if r[4].startswith('same move') else 'similar' if r[4].startswith('similar')
                                else 'kept' for r in res['rows'])
        tot.update(c)
        for r in res['rows']:
            if r[4].startswith('base kept'):
                missing[res['ps2_names'].get(r[2], r[3]) or '(id %d)' % r[2]] += 1
        rows.append((n, nm, c['same'], c['similar'], c['kept'], len(res['moves_txt'])))
        if limit and len(rows) >= limit:
            break
    for n, nm, a, b, k, d in rows:
        print('%4d %-24s same %3d similar %3d kept %3d  (%d slots differ from the base)' % (n, nm[:24], a, b, k, d))
    t = max(1, sum(tot.values()))
    print('%s: %d superstars, %d mapped slots: same move %d (%.0f%%), similar %d (%.0f%%), the base\'s kept %d (%.0f%%)'
          % (game, len(rows), t, tot['same'], 100.0 * tot['same'] / t, tot['similar'], 100.0 * tot['similar'] / t,
             tot['kept'], 100.0 * tot['kept'] / t))
    print('most kept moves:', '; '.join('%s %d' % kv for kv in missing.most_common(30)))
    return rows, tot, missing


def main(argv):
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('--write-maps', action='store_true')
    ap.add_argument('--report', nargs=2, metavar=('GAME', 'ISO'))
    ap.add_argument('--pac11')
    ap.add_argument('--limit', type=int)
    a = ap.parse_args(argv[1:])
    if a.write_maps:
        write_maps()
    if a.report:
        if not a.pac11:
            ap.error('--report needs --pac11 (the SvR 2011 pac folder)')
        report(a.report[0], a.report[1], a.pac11, a.limit)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
