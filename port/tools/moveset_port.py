"""A ported superstar's own moveset in SvR 2011 terms (superstar_port's moves step).

    port_moveset(ctx) -> {'moves_txt': [lines], 'rows': [...], 'pack': folder or None}

For each slot of the source game's profile that tools/moveset_maps.json maps to a 2011 slot:
  - fixed 2011 slots (the same id for the whole roster, pro_slots.json) stay as they are;
  - the source move translated by name (MOVS/WAZE) to a 2011 move whose motion is in the slot's
    category (pro_slots.json "bank") -> that move;
  - else, when the source has its motion and a porter exists (moves_port.py: WWE '12 / '13 / 2K14, SvR
    2007-2009 - their tracks copied onto 2011's own record of the move),
    the move is ported (the template method) into a move pack inside the mod, on a free 2011
    record - one per source move, picked by the superstar and move so two mods rarely meet;
  - else the base's move stays.
moves.txt holds the slots that differ from the base's profile (0xOFF=id). Every row is reported.
A 2011 move is used when it has a motion (some records are cut moves) and 2011's own roster has it in that
slot or in the same menu group (pro_slots.json "menu") - a strike doesn't land in a grapple slot.
Taunts (unless port_taunts is False - superstar_port --no-taunts) are ported on a taunt template (the base's taunt in that slot, else another 2011 taunt on YMBs
banks), on a free record in the template's thousand: moves_port's YMBs path. Submissions on YMBs banks
keep the base's.
"""
import collections
import glob
import json
import os
import shutil
import struct
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
N11 = 0x1C0 // 2


def log(*a):
    print(*a, file=sys.stderr, flush=True)


def stage_times(stages):
    """moves_port's progress steps [(text, time)] -> 'reading 0.1, decode 0.3, template pick 0.0, retarget 1.2, tracks 2.0'
    (tracks with a YMBs taunt: the limb solve + the encode)"""
    names = {'reading the game': 'reading', 'decoding the motion': 'decode', '2011 banks': 'template pick',
             'retargeting': 'retarget', 'tracks': 'tracks'}
    out, ymbs = [], False
    for (a, ta), (b, tb) in zip(stages, stages[1:]):
        if a.startswith('tracks:'):
            ymbs = True
            a = 'tracks'
        n = names.get(a)
        if n is None:
            continue
        if out and out[-1][0] == n:
            out[-1][1] += tb - ta
        else:
            out.append([n, tb - ta])
    return ', '.join('%s%s %.1f' % (n, ' (YMBs)' if n == 'tracks' and ymbs else '', t) for n, t in out)


def _norm(s):
    import moves_port as MP
    return MP.norm(s)


class Ctx:
    """gid, cid, base, g11 (superstar_port 2011 game), pro (source profile bytes), waze (source {id: name}),
    mgame (moves_port game for porting, or None), pac11, svrmod, work, mod, rep, port (bool), max_port"""

    def __init__(self, **kw):
        self.__dict__.update(kw)


_G11M = {}


def g11_moves(pac11, work):
    import moves_port as MP
    if pac11 not in _G11M:
        _G11M[pac11] = MP.Game11(pac11, os.path.join(work, '.mv11'))
    return _G11M[pac11]


def port_moveset(c):
    import moves_port as MP
    maps = json.load(open(os.path.join(HERE, 'moveset_maps.json')))
    if c.gid not in maps:
        return None
    smap = {int(s, 16): int(t, 16) for s, (t, sc) in maps[c.gid]['map'].items()}
    slots = {int(s['off']): s for s in json.load(open(os.path.join(HERE, 'pro_slots.json')))['slots']}
    w11 = c.g11.waze()
    by11 = collections.defaultdict(set)
    for i, (bits, nm) in w11.items():
        by11[_norm(nm)].add(i)
    g11m = g11_moves(c.pac11, c.work)
    moved = g11m.moved   # (2011 moves with a motion; some records are cut moves without one)
    # where 2011's own roster uses each move: by slot, and by the slot's menu group (pro_slots "menu")
    menu_of = {off: (sl.get('menu') or '') for off, sl in slots.items()}
    in_slot, in_menu = collections.defaultdict(set), collections.defaultdict(set)
    for r in c.g11.pro.values():
        if len(r) < 2 * N11:
            continue
        for k, x in enumerate(struct.unpack_from('<%dH' % N11, r, 0)):
            in_slot[x].add(2 * k)
            in_menu[x].add(menu_of.get(2 * k, ''))
    base = list(struct.unpack_from('<%dH' % N11, c.g11.pro[c.base], 0))
    area = list(base)
    rows = []
    to_port = collections.OrderedDict()   # source id -> [2011 offsets]
    src_index = c.mgame.index() if c.mgame is not None else {}
    templ = {}   # source id -> the 2011 taunt template (YMBs banks)

    def ymbs_taunt(mid):
        return mid in moved and any(kd == 'YMBs' for p, kd, ks in g11m.banks_of(mid))
    any_taunt = None
    for so, to in sorted(smap.items()):
        if so + 2 > len(c.pro) or to // 2 >= N11:
            continue
        v = struct.unpack_from('<H', c.pro, so)[0]
        sl = slots.get(to, {})
        nm = c.waze.get(v, '')
        if sl.get('fixed'):
            continue
        if not v or (v not in c.waze and v not in w11):   # (an empty slot - 0 - or no move record: the base's; 0 was
            continue                                    # "ported" as MOVE 0 into Rumble slots, Mankind '13)
        if getattr(c, 'select', None) is not None and to not in c.select:   # (moveset-pack --rows: these slots only)
            continue
        bank = (sl.get('bank') or '').strip()
        if getattr(c, 'same_ids', False) and v in w11:   # (SvR 2007-2009: 2011's ids)
            cand = [v]
        else:
            cand = sorted(by11.get(_norm(nm), set())) if nm else []
        fit = [x for x in cand if x in moved and MP.usable11(w11, x) and (to in in_slot[x] or menu_of.get(to, '') in in_menu[x])]
        # (a taunt - 9000-9999, as 2007-2011 keep them - only in a taunt slot, and the ring movement sets (BASE) only
        # as 2011 itself has them: SvR 2007's map put Booker T's and Kurt Angle's taunts in the ring in / out slots and
        # strike slots - in a Hell in a Cell they ran on forever or froze, report 20261101-180328)
        if 9000 <= v < 10000 and menu_of.get(to) != 'Taunts':
            rows.append((so, to, v, nm, 'base kept (a taunt in a %s slot)' % (menu_of.get(to) or 'move'), base[to // 2]))
            continue
        if bank == 'BASE' and not fit:
            rows.append((so, to, v, nm, 'base kept (a ring movement slot: 2011\'s own sets only)', base[to // 2]))
            continue
        if fit:
            area[to // 2] = fit[0]
            rows.append((so, to, v, nm, 'same move in 2011', fit[0]))
        elif menu_of.get(to) == 'Taunts' and v in src_index and not getattr(c, 'port_taunts', True):
            rows.append((so, to, v, nm, 'base kept (a taunt: --no-taunts)',
                         base[to // 2]))
        elif v in src_index and c.port and c.mgame is not None:
            if menu_of.get(to) == 'Taunts' and v not in templ:   # (a taunt plays from YMBs banks: a taunt template)
                t = base[to // 2] if ymbs_taunt(base[to // 2]) else None
                if t is None:
                    if any_taunt is None:
                        any_taunt = next((x for x in sorted(in_menu) if 'Taunts' in in_menu[x] and ymbs_taunt(x)), 0)
                    t = any_taunt or None
                if t is None:
                    rows.append((so, to, v, nm, 'base kept (no 2011 taunt template)', base[to // 2]))
                    continue
                templ[v] = t
            to_port.setdefault(v, []).append(to)
        else:
            rows.append((so, to, v, nm, 'base kept (%s)' % (
                ('no motion in 2011' if not any(x in moved for x in cand) else 'used elsewhere in 2011') if cand else
                                                             'no porter for this game yet' if c.mgame is None else
                                                             'no motion in the source' if v not in src_index else
                                                             'porting off'), base[to // 2]))
    pack = None
    if to_port:
        # (never a stock one's, nor one another mod's pack already defines: packs are merged by move id - Mankind's
        # signature 2437 was Maria's and Bob Orton's 2437 too, whose waze line took its signature bits, 29 Oct)
        taken = other_pack_ids(c.pac11, '%s %d' % (c.gid, c.cid))
        free = [i for i in sorted(w11) if i not in g11m.moved and i > 1000 and i not in in_slot and i not in taken]
        used = set(taken)   # (moves_port's own re-pick under a bank's id limit avoids them too: _avoid)
        # (a re-port gives each slot the id its last version had: the free pool shifts with what else is installed, so
        # the hashed pick alone moved Mankind 2.0.3's signatures / finishers off 2.0.2's ids)
        prev = own_pack_slots(c.pac11, '%s %d' % (c.gid, c.cid))
        pdir = os.path.join(c.work, 'moveset_port')
        if os.path.exists(pdir) and not getattr(c, 'dry', False):
            shutil.rmtree(pdir)
        lines, motions = [], []
        for k, (v, tos) in enumerate(to_port.items()):
            nm = c.waze.get(v, 'MOVE %d' % v)
            if c.max_port is not None and k >= c.max_port:
                for to in tos:
                    rows.append((None, to, v, nm, 'base kept (over the %d ported moves)' % c.max_port, base[to // 2]))
                continue
            if getattr(c, 'dry', False):   # (superstar_port moveset: what would be ported, nothing converted)
                for to in tos:
                    rows.append((None, to, v, nm, 'would port (template %s)' % (templ.get(v) or 'picked when porting'),
                                 base[to // 2]))
                continue
            h = zlib.crc32(('%s/%d/%d' % (c.gid, c.cid, v)).encode())
            new_id = None
            if getattr(c, 'same_ids', False) and v in w11 and v not in g11m.moved and v > 1000 and v not in used:
                new_id = v   # (SvR 2007-2009: 2011 keeps the move's own record, without a motion)
            if new_id is None:
                old = next((prev[to] for to in tos if prev.get(to) and prev[to] not in used), None)
                if old is not None and old in w11 and old not in g11m.moved and old not in in_slot:
                    new_id = old
            pool = free
            if v in templ:   # (a taunt: a record in the template's thousand - 9000-9999; elsewhere the match didn't find it)
                pool = [i for i in free if i // 1000 == templ[v] // 1000] or free
            for d in range(len(pool) if new_id is None else 0):
                cand = pool[(h + d) % len(pool)]
                if cand not in used:
                    new_id = cand
                    break
            if new_id is None:
                for to in tos:
                    rows.append((None, to, v, nm, 'base kept (no free 2011 record)', base[to // 2]))
                continue
            used.add(new_id)
            r = {'notes': [], 'warnings': [], 'dropped': []}
            a = dict(game=c.gid, item=str(v), out=os.path.join(pdir, str(v)), pac11=c.pac11, svrmod=c.svrmod,
                     id=str(new_id), _g=c.mgame, _g11=g11m, _avoid=used)
            if v in templ:
                a['template'] = str(templ[v])
            a['cat'] = (slots.get(tos[0]) or {}).get('bank') or ''   # (template categories: the slot's - moves_port)
            lo, hi = getattr(c, 'prog', (70, 85))
            nport = min(len(to_port), c.max_port if c.max_port is not None else len(to_port))
            kinds = ['taunt' if x in templ else 'move' for x in list(to_port)[:nport]]
            kind = kinds[k]
            line = '%s %d of %d: %s' % (kind, kinds[:k + 1].count(kind), kinds.count(kind), nm)
            print('progress %d %s' % (lo + (hi - lo) * k // max(1, nport), line), file=sys.stderr, flush=True)
            t0 = time.time()
            limit = float(os.environ.get('SVR2011_MOVE_TIME_LIMIT') or getattr(c, 'move_time', 120))
            MP.DEADLINE, MP.QUIET, MP.STAGES = t0 + limit, True, [('start', t0)]
            try:
                MP.convert(a, r)
            except MP.TimeLimit:
                for to in tos:
                    rows.append((None, to, v, nm, 'base kept (port over %d s: stopped)' % limit, base[to // 2]))
                c.rep['warnings'].append('moveset: "%s" took over %d s to port and was stopped - the base move kept'
                                         % (nm, limit))
                continue
            except Exception as e:   # noqa: BLE001
                for to in tos:
                    rows.append((None, to, v, nm, 'base kept (port failed: %s)' % str(e)[:80], base[to // 2]))
                continue
            finally:
                stages, MP.DEADLINE, MP.QUIET, MP.STAGES = MP.STAGES, None, False, None
            log('%s ported in %.1f s (%s)' % (line, time.time() - t0, stage_times(stages)))
            if r.get('id') and r['id'] != new_id:   # (moves_port moved it under its template banks' id limit)
                used.discard(new_id)
                new_id = r['id']
                used.add(new_id)
            for to in tos:
                area[to // 2] = new_id
                rows.append((None, to, v, nm, 'ported (template %s)' % r.get('template'), new_id))
            lines += open(os.path.join(r['folder'], 'pack.txt')).read().splitlines()
            # (a signature / finisher: the record needs categories 2 and 3 - every one of 2011's roster's has them; a
            # template without them left the slot empty - Mankind's Double Arm DDT 2 on Spin Fisherman Suplex 1)
            tb = w11.get(int(r.get('template') or 0), (b'', ''))[0]
            if any(to in SPECIAL_SLOTS for to in tos) and len(tb) == 16 and not (tb[0] & 0x0C) == 0x0C:
                bits = bytearray(tb)
                bits[0] |= 0x0C
                lines.append('waze %d %s' % (new_id, bits.hex()))
                c.rep['notes'].append('moveset: "%s" given the signature / finisher categories (its template %s had '
                                      'none)' % (nm, r.get('template')))
            motions.append(os.path.join(r['folder'], 'motions'))
        if lines:
            pack = os.path.join(c.work, 'moveset_pack')
            if os.path.exists(pack):
                shutil.rmtree(pack)
            os.makedirs(os.path.join(pack, 'motions'))
            for m in motions:
                for f in os.listdir(m):
                    shutil.copyfile(os.path.join(m, f), os.path.join(pack, 'motions', f))
            open(os.path.join(pack, 'pack.txt'), 'w', newline='\n').write('\n'.join(lines) + '\n')
    moves_txt = ['0x%03X=%d' % (k * 2, v) for k, v in enumerate(area) if v != base[k]]
    return {'moves_txt': moves_txt, 'rows': rows, 'pack': pack}


SPECIAL_SLOTS = (0x1E, 0x20, 0x22, 0x24)   # (2011's signature 1/2 and finisher 1/2 - pro_slots.json)


def other_pack_ids(pac11, source=None):
    """the move ids the move packs beside the game define (waze / wazecopy / wazename lines): the Bundled Mods'
    .svrmod files and the installed Mods' folders of the game folder that holds pac11. A pack whose manifest has
    source=<source> is the superstar being ported again and is left out (a re-port keeps its ids: Mankind 2.0.3,
    ported beside an installed 2.0.2, got four new signature / finisher ids - move-sets made on 2.0.2 lost them)"""
    import re
    import zipfile
    game = os.path.dirname(os.path.abspath(pac11))
    pat = re.compile(r'^(?:waze|wazecopy|wazename)\s+(\d+)\s', re.M)
    mine = re.compile(r'^source\s*=\s*%s\s*$' % re.escape(source), re.M) if source else None
    ids = set()
    for f in glob.glob(os.path.join(game, 'Bundled Mods', '*.svrmod')):
        try:
            with zipfile.ZipFile(f) as z:
                names = z.namelist()
                if mine and any(n.endswith('manifest.txt') and mine.search(z.read(n).decode('utf-8', 'replace'))
                                for n in names):
                    continue
                for n in names:
                    if n.endswith('pack.txt'):
                        ids.update(int(x) for x in pat.findall(z.read(n).decode('utf-8', 'replace')))
        except (OSError, zipfile.BadZipFile):
            continue
    for f in glob.glob(os.path.join(game, 'Mods', '**', 'pack.txt'), recursive=True):
        try:
            man = os.path.join(os.path.dirname(os.path.dirname(f)), 'manifest.txt')   # (<mod>/moves/pack.txt)
            if mine and os.path.exists(man) and mine.search(open(man, encoding='utf-8', errors='replace').read()):
                continue
            ids.update(int(x) for x in pat.findall(open(f, encoding='utf-8', errors='replace').read()))
        except OSError:
            continue
    return ids


def own_pack_slots(pac11, source):
    """{moveset offset: move id} of the slots the superstar's installed version (manifest source=<source>, in the
    Bundled Mods or the Mods) filled with its own ported moves (ids its pack copies - wazecopy - or sets the
    category bits of - waze: an SvR 2007-2009 move on its own 2011 record) - {} if none"""
    import re
    import zipfile
    game = os.path.dirname(os.path.abspath(pac11))
    mine = re.compile(r'^source\s*=\s*%s\s*$' % re.escape(source), re.M)
    texts = []
    for f in glob.glob(os.path.join(game, 'Bundled Mods', '*.svrmod')):
        try:
            with zipfile.ZipFile(f) as z:
                rd = {n: z.read(n).decode('utf-8', 'replace') for n in z.namelist()
                      if n.endswith(('manifest.txt', 'moves.txt', 'pack.txt'))}
        except (OSError, zipfile.BadZipFile):
            continue
        texts.append(rd)
    for man in glob.glob(os.path.join(game, 'Mods', '**', 'manifest.txt'), recursive=True):
        d = os.path.dirname(man)
        rd = {}
        for n in ('manifest.txt', 'moves.txt', os.path.join('moves', 'pack.txt')):
            try:
                rd[n.replace(os.sep, '/')] = open(os.path.join(d, n), encoding='utf-8', errors='replace').read()
            except OSError:
                pass
        texts.append(rd)
    for rd in texts:
        if not any(n.endswith('manifest.txt') and mine.search(t) for n, t in rd.items()):
            continue
        copied = {int(x) for n, t in rd.items() if n.endswith('pack.txt')
                  for x in re.findall(r'^(?:wazecopy|waze)\s+(\d+)\s', t, re.M)}
        out = {}
        for n, t in rd.items():
            if n.endswith('moves.txt') and not n.endswith('pack.txt'):
                for o, i in re.findall(r'^0x([0-9A-Fa-f]+)=(\d+)\s*$', t, re.M):
                    if int(i) in copied:
                        out[int(o, 16)] = int(i)
        if out:
            return out
    return {}


def merge_pack(pack, mod):
    """a move pack into the mod's moves/ (the entrance pack may be there already: lines appended)"""
    dst = os.path.join(mod, 'moves')
    os.makedirs(os.path.join(dst, 'motions'), exist_ok=True)
    for f in os.listdir(os.path.join(pack, 'motions')):
        shutil.copyfile(os.path.join(pack, 'motions', f), os.path.join(dst, 'motions', f))
    old = open(os.path.join(dst, 'pack.txt')).read() if os.path.exists(os.path.join(dst, 'pack.txt')) else ''
    new = open(os.path.join(pack, 'pack.txt')).read()
    open(os.path.join(dst, 'pack.txt'), 'w', newline='\n').write(old + ('' if not old or old.endswith('\n') else '\n') + new)


def summary(res):
    c = collections.Counter(r[4].split(' (')[0] for r in res['rows'])
    return ', '.join('%s %d' % kv for kv in sorted(c.items()))
