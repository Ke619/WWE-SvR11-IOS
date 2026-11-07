"""Moves from other WWE games as SvR 2011 move packs - the converter the Mod
Maker calls (docs/MOD_MAKER_3_PLAN.md section 4, docs/OTHER_GAMES.md,
docs/MOVE_PACKS.md).

    py moves_port.py games
    py moves_port.py list    --game <id> --src <ISO or extract folder> [--pac11 <2011 pac>]
    py moves_port.py convert --game <id> --src <...> --item <move id> --out <dir> [options]
    py moves_port.py renumber --mod <installed mod or move pack folder> [--pac11 <2011 pac>] [--mods <Mods>]
                     [--dry 1]
            an installed mod's moves on ids of 16000 and up (older ports) moved to free records (free_ids) below
            their banks' last ids: the game leaves such a body motion out of MOT/BM00 (move_packs.cpp) and the
            move played as nothing - the wrestler slid off and froze (report a745b2, Sin Cara's 30073). pack.txt,
            the motions' follow-ups (op 49), the moves list(s) and the manifest's version change; a backup zip of
            the files before goes into tools/work/renumber_backup. --dry 1: the plan only.

stdout is only JSON; logs go to stderr, with progress lines `progress <0-100> <text>`.
Exit 0 ok, 2 bad input, 3 failed.

convert writes a move pack <dir>/<pack>/ (pack.txt + motions/, docs/MOVE_PACKS.md; install
as Mods/Moves/<pack> or a superstar mod's moves/ folder) and <dir>/report.json:
    {ok, out, folder, opens: "moves", id, name, template, warnings[], dropped[], notes[]}
The move gets a 2011 move id (`id`): a superstar mod's moves.txt can name it.

Options (convert):
    --template N   the 2011 move whose record, per-key data, events and banks the new move takes
                   (default: the 2011 move of the same category whose length is closest)
    --id N         the 2011 record to use (default: a free one - a record 2011 keeps without
                   motions - with the same name if there is one, else the first free one)
    --name TEXT    the move's name (default: the source game's)

Games:
    wwe12   WWE '12 - motions are WWE '13's ZMOT/YANM curves on the Biped rig: decoded by
            yanm_decode.py and retargeted to 2011's body bones by w13_retarget.py (the
            HCTP Entrance chat's, shared); WWE '12 bodies are plain FK (ik=False).
    wwe13   WWE '13 - the same, in xcompressed pacs (svrmod xdec: --svrmod or found as
            wwe13_entrance.py finds it); its bodies use the IK stream
    wwe2k14 WWE 2K14 - the same; move records in createmode.pac, categories coded (templates from
            every category)
    svr2007 / svr2008 / svr2009  SvR 2007-2009 - their m.pac banks are 2011's YMKs banks without the
            16-byte header (same 0x7d body records, same move ids as 2011): the tracks are copied (Sabu's
            make08 method) - every x / y of the move, 2008-only event ops 192 / 193 dropped, jumps to the
            move pointed at the new id - into the banks of a 2011 template of the same kind (its record,
            EXH / sound / MBD records, sound frames scaled to the new length). Moves on the old YMBs banks
            (submissions, taunts) aren't read yet.

How a move is made (the WWE '13 Mankind moves' method): the attacker (y 0) and the victim
(y 1) of every x the template has are the source's blocks 0 and 1, one key a frame at 30 Hz,
the attacker's start placed where the template's starts (both tracks moved alike: the victim
keeps the source's place relative to the attacker); prop and camera tracks and reversal
branches (x 90-92) are the template's. The template's in-stream events are moved in time
with the new length, its jumps to itself point to the new move. Its record (pack `wazecopy`),
name (`wazename`), per-key EXH / sound / MBD records (exh / evt / mbd) go with it. Not
carried: finger poses (the template's hands), the source's own sounds and damage timing
(the template's, retimed). A template on YMBs banks (a taunt; moveset_port passes the base's) gets the
source's attacker (and victim) as YMBs channels: ymbs_retarget.py solves the game's limb IK + rotations
(worst joint error in the notes), ymbs.py encodes; the motion keeps the template's header times.
"""
import json
import math
import os
import re
import struct
import sys
import time
import traceback
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ch_tool  # noqa: E402
import svrfmt  # noqa: E402

PAC11 = os.path.join(os.path.dirname(os.path.dirname(HERE)), 'Game Files', 'pac')

GAMES = [
    dict(id='wwe12', name="WWE '12", platform='x360', disc_hint='54510890', status='ready'),
    dict(id='wwe13', name="WWE '13", platform='x360', disc_hint='545108B4', status='ready'),
    dict(id='wwe2k14', name='WWE 2K14', platform='x360', disc_hint='545408B2', status='ready'),
    dict(id='wwe2k15', name='WWE 2K15', platform='x360', disc_hint='545408B6', status='ready'),
    dict(id='wwe2k16', name='WWE 2K16', platform='x360', disc_hint='315A07D2', status='ready'),
    dict(id='wwe2k17', name='WWE 2K17', platform='x360', disc_hint='545408BA', status='ready'),
    dict(id='svr2010', name='WWE SmackDown vs. Raw 2010', platform='x360', disc_hint='54510844', status='research'),
    dict(id='svr2009', name='WWE SmackDown vs. Raw 2009', platform='x360', disc_hint='54510826', status='ready'),
    dict(id='svr2008', name='WWE SmackDown vs. Raw 2008', platform='x360', disc_hint='5451080B', status='ready'),
    dict(id='svr2007', name='WWE SmackDown vs. Raw 2007', platform='x360', disc_hint='545107E0', status='ready'),
]
OLD = ('svr2007', 'svr2008', 'svr2009')
READY = {g['id'] for g in GAMES if g['status'] == 'ready'}
# WWE '12 ZMVM category -> SvR 2011 MVMT bank (moveset menu categories)
CATS = {'STAT': 'STAT', 'BASE': 'BASE', 'STN1': 'STN1', 'STN2': 'STN2', 'GRND': 'GRND', 'CORN': 'CORN',
        'CETB': 'CETB', 'ROPE': 'ROPE', 'APRO': 'APRO', 'RUN ': 'RUN ', 'TEAM': 'TEAM', 'FINI': 'FINI',
        'SPE1': 'STN1', 'SPE2': 'STN2', 'ANAS': 'ANAS', 'RUMB': 'RUMB', 'LADR': 'LADR', 'CELL': 'CELL',
        'ABIL': 'ABIL', 'DIVI': 'CORN', 'APR2': 'APRO', 'ANA2': 'ANAS', 'WEAP': 'STAT', 'APR1': 'STN1', 'SPE0': 'STN1'}
# (APR1 - WWE '12 / '13's grapples by the apron: Canadian Backbreaker 1, Emerald Fusion 1, F-5 1 - are standing
# grapples in their motion; on 2011's APRO templates (Apron Guillotine Drop 10885) the template's own events, the
# victim's fall from the apron to the floor, put both at floor height: they sank under the ring, 27 Oct)


# Games whose two-person moves keep the attacker turned 180 degrees in its own frame, this many root units down -z
# from the victim's (w13_retarget.facing_frame). 2K15: its Attitude Adjustment 2 (2011's 5010 reused) matched
# 2011's attacker only turned; unturned, the victim played its part metres away (docs/MOVE_PORTING.md).
FACING_GAP = {'wwe2k15': 1500, 'wwe13': 1500, 'wwe12': 1500,   # (WWE '13: the same layout - its AA 50285 matches
              'wwe2k14': 1500, 'wwe2k16': 1500, 'wwe2k17': 1500}  # 2011's 5010 only turned; '12, 2K14, 2K16 and 2K17
# alike: their AA 50285 starts 142-160 units apart turned (2011's own: 229), 1641 as stored - a 2K17 Blue Thunder
# Bomb ported before this started its pair metres apart, "the animation skipped and Edge froze")
# 2011's own pair layout (SVR2011_TEST_POS_LOG, 29 Oct): each person's body = his base + his root turned by his facing,
# nothing subtracted; the victim always faces the attacker (facings 180 degrees apart - Texas Piledriver, Powerslam 6,
# Flip Bottom, Tiger Bomb 2) and a standing grapple's bases end up this many root units apart, straight ahead
PAIR_GAP = 1500


class BadInput(Exception):
    pass


def log(*a):
    print(*a, file=sys.stderr, flush=True)


DEADLINE = None   # (time.time() limit for one convert, set by callers like moveset_port; checked between steps)
QUIET = False     # (no progress lines: a caller prints its own)
STAGES = None     # (a list: each progress step's (text, time) is added - moveset_port's per-move timings)


class TimeLimit(Exception):
    pass


# (a move on YMBs banks - a submission, a taunt - solves the limb IK per phase and bank: 2K15's Kimura Lock (5 hold
# phases) took 184 s and moveset_port's 120 s a move stopped it - the base move kept, "missing from CREATE A MOVE-SET",
# report 20261104-093125. Such a move gets this much more once.)
YMBS_EXTRA = 480


def extend_deadline():
    global DEADLINE
    if DEADLINE is not None:
        DEADLINE += YMBS_EXTRA


def progress(pct, text):
    if STAGES is not None:
        STAGES.append((text, time.time()))
    if DEADLINE is not None and time.time() > DEADLINE:
        raise TimeLimit('over the time limit at "%s"' % text)
    if not QUIET:
        print('progress %d %s' % (pct, text), file=sys.stderr, flush=True)


def norm(s):
    s = s.lower().replace('_', ' ').strip()
    return re.sub(r'[^a-z0-9]', '', re.sub(r'^\d+ ', '', s))


def read_pac(raw):
    """EPAC / EPK8 groups [(type, [(name, data)])] whichever the entry size: bit 0 of a group's
    flags (u16 after the count) picks 16-byte entries with 8-character names, else 12-byte with 4;
    the magic doesn't decide it (WWE '13's misc.pac says EPK8 with 12-byte entries, 2K14's ch pacs
    EPAC with 16-byte ones); a table that doesn't read is tried the other way"""
    def parse(prefer_wide):
        out, q = [], 0x800
        while q + 12 <= 0x4000:
            typ = raw[q:q + 4]
            if typ == b'\0\0\0\0':
                break
            cnt, flags = struct.unpack_from('<HH', raw, q + 4)
            wide = bool(flags & 1) if prefer_wide is None else prefer_wide
            esz = 16 if wide else 12
            n = cnt // (4 if wide else 3)
            q += 12
            ents = []
            for _ in range(n):
                if q + esz > 0x4000:
                    raise ValueError('table')
                nm = raw[q:q + esz - 8]
                sec, sz = struct.unpack_from('<II', raw, q + esz - 8)
                o = 0x4000 + sec * 0x800
                if o + sz * 0x100 > len(raw) + 0x800:
                    raise ValueError('entry')
                ents.append((nm.split(b'\0')[0].strip(), raw[o:o + sz * 0x100]))
                q += esz
            out.append((typ, ents))
        return out
    for pref in (None, False, True):
        try:
            return parse(pref)
        except (ValueError, struct.error):
            continue
    raise ValueError('not a pac table')


def cache_dir(pac11, fallback):
    """a writable folder kept between converts: <game>\\Mods\\.convert\\cache (the Port page's "Clear it" empties
    it), else %LOCALAPPDATA%\\SvR2011\\convert cache, else fallback (the Mod Maker Tools folder may be read-only
    and is replaced on update)"""
    cands = [os.path.join(os.path.dirname(os.path.abspath(pac11)), 'Mods', '.convert', 'cache')]
    if os.environ.get('LOCALAPPDATA'):
        cands.append(os.path.join(os.environ['LOCALAPPDATA'], 'SvR2011', 'convert cache'))
    for d in cands + [fallback]:
        try:
            os.makedirs(d, exist_ok=True)
            probe = os.path.join(d, '.write_test')
            open(probe, 'wb').close()
            os.remove(probe)
            return d
        except OSError:
            continue
    return fallback


def save_atomic(path, data=None, obj=None):
    """write bytes (data) or a pickle (obj) to path whole or not at all (a cancelled convert - the Mod Maker kills
    the process tree - leaves no half-written cache)"""
    import pickle
    tmp = path + '.part'
    with open(tmp, 'wb') as f:
        if obj is not None:
            pickle.dump(obj, f)
        else:
            f.write(data)
    os.replace(tmp, path)


def placeholder(raw):
    """a pac whose table is one "DMMY" entry (no group header; a 10 KB PACH with no moves): the disc's stand-in
    for DLC motions (WWE 2K15's dlc_mot01-03.pac, 30 KB each) - the DLC packages bring the real ones"""
    return raw[:4] in (b'EPK8', b'EPAC') and raw[0x800:0x808] == b'DMMY    ' and not any(raw[0x810:0x830])


def unz(b):
    """a pac entry: BPE (Yuke's) or ZLIB ("ZLIB", u32, u32 packed, u32 size, zlib data at +16)"""
    if b[:4] == b'ZLIB':
        return zlib.decompress(b[16:])
    return ch_tool.unpack(b)


# --------------------------------------------------------------------------- WWE '12

class Game12:
    """WWE '12: misc.pac MOVS/WAZE (160-byte records from 0x430: name +0x10, id +0x90);
    m.pac ZMVM/<category>: PACH trees ... -> x -> move id -> a '13 motion record (YANM blocks)."""

    def __init__(self, src):
        self.src = src
        g = read_pac(self.pac_bytes('pac/misc.pac'))
        raw = unz(dict(((t, n), b) for t, es in g for n, b in es)[(b'MOVS', b'WAZE')])
        self.waze = waze_records(raw)
        self._m = None
        self._index = None
        self.merge_dlc_waze()

    def pac_bytes(self, rel):
        return open(self.src.need(rel), 'rb').read()

    def motion_pacs(self):
        return ['pac/m.pac']

    # ---- the game's DLC packages (superstar_port --dlc): moves the disc has no motion for
    @staticmethod
    def _read(raw):
        return read_pac(raw)

    def decode(self, raw):
        return raw

    def dlc_packages(self):
        dlc = getattr(self.src, 'dlc', None)
        return dlc.packages if dlc else []

    def merge_dlc_waze(self):
        """the packages' move records: their names, for the ids the disc's table lacks. Each package has a table of its own:
        pac/misc.pac ('12), misc_dlc<nn> MOVS/WAZE ('13, 2K14), misc01_start_dlc<nn> MOVS/WA20, WAZ3, WA50 (2K15), createmode_dlc<nn>
        MOVS/DL<nn> (2K16: its own new moves only)"""
        for pk in self.dlc_packages():
            for low in pk.matching(r'^pac/(misc|createmode)[^/]*\.pac$'):
                try:
                    groups = self._read(self.decode(pk.read(low)))
                except (ValueError, IndexError, KeyError, OSError, struct.error) as e:
                    log('moves_port: %s in %s skipped: %s' % (low, pk.name, e))
                    continue
                for t, es in groups:
                    if t != b'MOVS':
                        continue
                    for n, b in es:
                        if n.strip(b'\0 ')[:2] not in (b'WA', b'DL'):
                            continue
                        try:
                            for mid, nm in waze_records_any(unz(b)).items():
                                if mid:
                                    self.waze.setdefault(mid, nm)
                        except (ValueError, IndexError, KeyError, struct.error) as e:
                            log('moves_port: %s %s in %s skipped: %s' % (low, n, pk.name, e))

    def dlc_groups(self):
        """[(group type, name, data)] of the packages' motion pacs: dlc_mot<nn> ('13, 2K14), m_dlc<nn> ('12), mot_dlc<nn> (2K15, 2K16).
        The tree is the same in each: ... -> x -> move id -> a motion record. ZMVM and ZMOT everywhere; 2K16's new moves are MOT/DL<nn>."""
        if not hasattr(self, '_dlc_groups'):
            self._dlc_groups = []
            for pk in self.dlc_packages():
                for low in pk.matching(r'^pac/(dlc_mot\d+|mot_dlc\d+|m_dlc\d+)\.pac$'):
                    try:
                        for t, es in self._read(self.decode(pk.read(low))):
                            for n, b in es:
                                n = n.strip(b'\0 ')
                                if t in (b'ZMVM', b'ZMOT') or (t.strip() == b'MOT' and n[:2] == b'DL'):
                                    self._dlc_groups.append((t, n, b))
                    except (ValueError, IndexError, KeyError, OSError, struct.error) as e:
                        self.skipped.append('%s in %s (%s)' % (low, pk.name, str(e)[:120]))
                        log('moves_port: %s in %s skipped: %s' % (low, pk.name, e))
        return self._dlc_groups

    def index_dlc(self):
        """the moves only the packages have: a record of theirs names the move and the disc has no motion for it
        (a package's trees also hold copies of disc moves and motions that aren't moves: left out)"""
        disc = set(self._index)
        for t, n, b in self.dlc_groups():
            cat = n.decode('latin1')

            def walk(raw, path):
                if raw[:4] == b'PACH':
                    for i, d in svrfmt.pach_read(raw):
                        try:
                            walk(unz(d), path + [i])
                        except Exception:   # noqa: BLE001
                            pass
                elif len(path) >= 2 and path[-1] not in disc and path[-1] in self.waze:
                    self._index.setdefault(path[-1], {}).setdefault(path[-2], (cat, path))
            try:
                walk(unz(b), [])
            except Exception as e:   # noqa: BLE001
                log('moves_port: DLC group %s skipped: %s' % (cat, e))

    def _groups(self):
        if self._m is None:
            self._m = []
            self.skipped, self.placeholders = [], []
            for rel in self.motion_pacs():
                try:
                    raw = self.pac_bytes(rel)
                    if placeholder(raw):
                        self.placeholders.append(rel)
                        continue
                    self._m += read_pac(raw)
                except Exception as e:   # noqa: BLE001  (a pac svrmod can't decode: its moves are left out)
                    self.skipped.append('%s (%s)' % (rel, str(e)[:120]))
                    log('moves_port: %s skipped: %s' % (rel, e))
        return self._m

    def index(self):
        """{move id: {x: (category, [path ids])}} over ZMVM"""
        if self._index is None:
            self._index = {}
            for t, es in self._groups():
                if t != b'ZMVM':
                    continue
                for n, b in es:
                    cat = n.decode('latin1')
                    try:
                        top = unz(b)
                    except Exception:   # noqa: BLE001
                        continue

                    def walk(raw, path):
                        if raw[:4] == b'PACH':
                            for i, d in svrfmt.pach_read(raw):
                                try:
                                    walk(unz(d), path + [i])
                                except Exception:   # noqa: BLE001
                                    pass
                        elif len(path) >= 2:
                            self._index.setdefault(path[-1], {}).setdefault(path[-2], (cat, path))
                    walk(top, [])
            self.index_dlc()
        return self._index

    def record(self, cat, path):
        name = cat.encode('latin1')
        blobs = [bb for t, es in self._groups() if t == b'ZMVM' for n, bb in es if n == name]
        blobs += [b for t, n, b in self.dlc_groups() if n == name]   # (two packages can have a group of the same name)
        for b in blobs:
            try:
                raw = unz(b)
                for i in path:
                    raw = unz(dict(svrfmt.pach_read(raw))[i])
                return raw
            except KeyError:
                continue
        raise KeyError('%s %s' % (cat, path))


class Game13(Game12):
    """WWE '13: the same motions and move records, in xcompressed (0x0FF512ED) pacs - decoded by
    the Mod Maker's svrmod xdec (wwe13_entrance.find_svrmod / xdec); misc.pac MOVS/WAZE (an "EPK8"
    with 12-byte entries); ZMVM in mot06-08.pac (and mot0x / dlc_mot ones that have it)."""

    def __init__(self, src, svrmod=None):
        import wwe13_entrance as WE
        self.WE = WE
        self.svrmod = WE.find_svrmod(svrmod)
        Game12.__init__(self, src)

    def pac_bytes(self, rel):
        return self.decode(open(self.src.need(rel), 'rb').read())

    def decode(self, raw):
        return self.WE.xdec(raw, self.svrmod) if raw[:4] == b'\x0f\xf5\x12\xed' else raw

    def motion_pacs(self):
        return ['pac/' + n for n in sorted(self.src.d.listdir('pac')) if re.match(r'(dlc_)?mot\d+\.pac$', n.lower())]


class Game14(Game13):
    """WWE 2K14: as WWE '13, its move records in createmode.pac MOVS/WAZE (the name 8 bytes earlier
    in the record: waze_records finds the grid), ZMVM in mot07 / mot08.pac with categories renamed to
    codes (C000, M001, ...): templates are looked for in every category."""

    def __init__(self, src, svrmod=None):
        import wwe13_entrance as WE
        self.WE = WE
        self.svrmod = WE.find_svrmod(svrmod)
        self.src = src
        g = read_pac(self.pac_bytes('pac/createmode.pac'))
        raw = unz(next(b for t, es in g if t == b'MOVS' for n, b in es if n == b'WAZE'))
        self.waze = waze_records(raw)
        self._m = None
        self._index = None
        self.merge_dlc_waze()


class Game2K15(Game14):
    """WWE 2K15: as 2K14 - createmode.pac MOVS/WAZE, ZMVM in mot07 / mot08.pac (YANM records)."""


class Game2K16(Game14):
    """WWE 2K16: as 2K14, its pacs' tables in 20-byte entries (superstar_port.epac20) where read_pac fails
    (createmode.pac, mot07 / mot08.pac)."""

    def __init__(self, src, svrmod=None):
        import wwe13_entrance as WE
        self.WE = WE
        self.svrmod = WE.find_svrmod(svrmod)
        self.src = src
        g = self._read(self.pac_bytes('pac/createmode.pac'))
        raw = unz(next(b for t, es in g if t == b'MOVS' for n, b in es if n.strip(b'\0 ') == b'WAZE'))
        self.waze = waze_records(raw)
        self._m = None
        self._index = None
        self.merge_dlc_waze()

    @staticmethod
    def _read(raw):
        try:
            return read_pac(raw)
        except ValueError:
            return __import__('superstar_port').epac20(raw)

    def _groups(self):
        if self._m is None:
            self._m = []
            self.skipped, self.placeholders = [], []
            for rel in self.motion_pacs():
                try:
                    raw = self.pac_bytes(rel)
                    if placeholder(raw):
                        self.placeholders.append(rel)
                        continue
                    self._m += [(t, [(n.strip(b'\0 '), b) for n, b in es]) for t, es in self._read(raw)]
                except Exception as e:   # noqa: BLE001
                    self.skipped.append('%s (%s)' % (rel, str(e)[:120]))
                    log('moves_port: %s skipped: %s' % (rel, e))
        return self._m


class Game2K17(Game14):
    """WWE 2K17: HSPC pacs (superstar_port.hspc / shdc; names hashed). Move names: createmode.pac's entry
    that reads as a MOVS/WAZE table (stored, not compressed). Motions: pac/ingame/motion/all_mot<N>.pac,
    each ids N * 10000.. : SHDC trees (thousands -> hundreds -> the move id) whose leaves are the 2K14
    motion records (YANM blocks); one variant per move. index() keys them {move id: {0: (pac, path)}}."""

    MOTION_DIR = 'pac/ingame/motion'

    def __init__(self, src, svrmod=None):
        import wwe13_entrance as WE
        self.WE = WE
        self.svrmod = WE.find_svrmod(svrmod)
        self.src = src
        sp = __import__('superstar_port')
        self.SP = sp
        tables = [waze_records(b) for _, b in sp.hspc(self.pac_bytes('pac/createmode.pac')) if len(b) > 4000]
        self.waze = max(tables, key=len) if tables else {}
        self._m = None
        self._index = None
        self._raw = {}

    def motion_pacs(self):
        return [self.MOTION_DIR + '/' + n for n in sorted(self.src.d.listdir(self.MOTION_DIR))
                if re.match(r'all_mot\d+\.pac$', n.lower())]

    def _tops(self, rel):
        if rel not in self._raw:
            self._raw[rel] = self.SP.hspc(self.pac_bytes(rel))
        return self._raw[rel]

    def index(self):
        if self._index is None:
            self._index = {}
            self.skipped = []

            def walk(b, rel, path, depth):
                if b[:4] in (b'BPE ', b'ZLIB'):
                    b = unz(b)
                if b[:4] == b'SHDC' and depth < 6:
                    for i, x in self.SP.shdc(b):
                        walk(x, rel, path + [i], depth + 1)
                elif path and b.count(b'YANM'):
                    self._index.setdefault(path[-1], {}).setdefault(0, (rel, path))
            for rel in self.motion_pacs():
                try:
                    for k, (_, b) in enumerate(self._tops(rel)):
                        walk(b, rel, [k], 0)
                except Exception as e:   # noqa: BLE001
                    self.skipped.append('%s (%s)' % (rel, str(e)[:120]))
                    log('moves_port: %s skipped: %s' % (rel, e))
        return self._index

    def record(self, cat, path):
        b = self._tops(cat)[path[0]][1]
        for i in path[1:]:
            if b[:4] in (b'BPE ', b'ZLIB'):
                b = unz(b)
            b = dict(self.SP.shdc(b))[i]
        return unz(b) if b[:4] in (b'BPE ', b'ZLIB') else b


# the source game classes by game id (wwe12's takes only src)
GAME_CLASS = {'wwe12': Game12, 'wwe13': Game13, 'wwe2k14': Game14, 'wwe2k15': Game2K15, 'wwe2k16': Game2K16,
              'wwe2k17': Game2K17}


class Game0789:
    """SvR 2007-2009: misc.pac MOVS/WAZE (144-byte records, name +0x10, id +0x88); every m.pac bank whose
    data reads as a YMKs bank once given the 16-byte header (the old YMBs ones don't)."""

    def __init__(self, src, work=None):
        import pickle
        import svr10_moves as SM
        self.src, self.SM = src, SM
        # (SvR 2009's DLC packages each hold the whole m.pac and misc.pac, the disc's and the DLC moves: taken when there -
        # the same banks with more in them; the moves' names start "DLC ")
        dlc = getattr(src, 'dlc', None)
        whole = bool(dlc and dlc.exists('pac/m.pac') and dlc.exists('pac/misc.pac'))
        g = read_pac(open(dlc.get('pac/misc.pac') if whole else src.need('pac/misc.pac'), 'rb').read())
        raw = unz(dict(((t, n), b) for t, es in g for n, b in es)[(b'MOVS', b'WAZE')])
        self.waze = {}
        self.situation = {}   # (where the move starts: +0x73, as 2011's WAZE - the same values for the same moves)
        for q in range(0, len(raw) - 143, 144):
            nm = raw[q + 0x10:q + 0x50].split(b'\0')[0]
            if len(nm) > 2 and all(32 <= c < 127 for c in nm):
                mid = struct.unpack_from('<H', raw, q + 0x88)[0]
                self.waze.setdefault(mid, re.sub(r'^DLC ', '', nm.decode('latin1').strip()))
                self.situation.setdefault(mid, raw[q + 0x73])
        if whole:   # (each package's table has the DLC moves up to it: Roster Update 2's the most)
            for pk in dlc.packages:
                if 'pac/misc.pac' in pk.files:
                    d = dict(((t, n), x) for t, es in read_pac(pk.read('pac/misc.pac')) for n, x in es)[(b'MOVS', b'WAZE')]
                    d = unz(d)
                    for q in range(0, len(d) - 143, 144):
                        nm = d[q + 0x10:q + 0x50].split(b'\0')[0]
                        if len(nm) > 2 and all(32 <= c < 127 for c in nm):
                            mid = struct.unpack_from('<H', d, q + 0x88)[0]
                            self.waze.setdefault(mid, re.sub(r'^DLC ', '', nm.decode('latin1').strip()))
                            self.situation.setdefault(mid, d[q + 0x73])
        self.groups = SM.read_epac(dlc.get('pac/m.pac') if whole else src.need('pac/m.pac'))[1]
        cache = os.path.join(work, 'index0789_dlc.pkl' if whole else 'index0789.pkl') if work else None
        if cache and os.path.exists(cache):
            self.banks = pickle.load(open(cache, 'rb'))
        else:
            self.banks = {}   # path -> [(id, x, y, frames)]
            for t, es in self.groups:
                for n, b in es:
                    self._walk(SM.unpack(b), [t.decode('latin1').strip(), n.decode('latin1').strip()])
            if cache:
                os.makedirs(work, exist_ok=True)
                pickle.dump(self.banks, open(cache, 'wb'))
        self._index = None
        self.skipped = []

    @staticmethod
    def as_bank(raw):
        import ymks
        if len(raw) < 0x104 or raw[:4] in (b'PACH', b'YMKs', b'YMBs', b'PAC '):
            return None
        n = struct.unpack_from('<I', raw, 0x100)[0]
        if not 0 < n < 20000 or 0x104 + 16 * n > len(raw):
            return None
        try:
            return ymks.read_bank(b'YMKs' + struct.pack('<I', 0x100) + bytes(8) + raw)
        except Exception:   # noqa: BLE001
            return None

    def _walk(self, raw, path):
        if raw[:4] == b'PACH':
            for i, d in svrfmt.pach_read(raw):
                self._walk(self.SM.unpack(d), path + ['%x' % i])
            return
        b = self.as_bank(raw)
        if b is not None:
            self.banks['/'.join(path)] = [(e['id'], e['x'], e['y'], e['frames']) for e in b.entries]

    def bank(self, path):
        parts = path.split('/')
        blob = self.SM.entry_get(self.groups, parts[0].ljust(4).encode(), parts[1].ljust(4).encode())
        raw = self.SM.tree_get(blob, [int(x, 16) for x in parts[2:]])
        return self.as_bank(raw)

    def index(self):
        """{move id: {x: (bank group, path)}} - per move, the bank with the most of its tracks"""
        if self._index is None:
            best = {}
            for path, ents in self.banks.items():
                cnt = {}
                for mid, x, y, fr in ents:
                    cnt[mid] = cnt.get(mid, 0) + 1
                for mid, c in cnt.items():
                    if c > best.get(mid, (0, ''))[0] or (c == best[mid][0] and path.startswith('MOT/')):
                        best[mid] = (c, path)
            self._index = {}
            for mid, (c, path) in best.items():
                for m2, x, y, fr in self.banks[path]:
                    if m2 == mid:
                        self._index.setdefault(mid, {})[x] = (path.split('/')[1], path)
        return self._index

    def packed_bank(self, mid):
        """SvR 2009's own match copy of the move (MOTP/GAME/<child>, the packed codec 2011's MOTP uses: the people
        packed, cameras / ropes / props as in the keyframe bank) - None for 2007 / 2008 (their MOT/GAME is keyframes)"""
        for path, ents in self.banks.items():
            if path.startswith('MOTP/') and any(m == mid for m, x, y, fr in ents):
                return path
        return None

    def menu_categories(self, mid):
        """the 2011 MVMT categories of the source's own menu banks for the move (2009: STND / DIVI / ROPE ...;
        2007 / 2008 file them all under MOTN / REMT - nothing)"""
        out = []
        for path, ents in self.banks.items():
            if path.startswith('MVMT/') and any(m == mid for m, x, y, fr in ents):
                c = path.split('/')[1].strip()
                for c11 in {'STND': ('STN1', 'STN2'), 'DIVI': ('CORN',)}.get(c, (c,)):
                    if c11 in SINGLES and c11 not in out:
                        out.append(c11)
        return out


def clean_events08(evs, old, new, keep=(192, 193), cues=None):
    """event ops 192 / 193 dropped unless in `keep` (2007-2009 and 2011 banks all size them 2 / 3; dropped only where
    the 2011 bank sizes them otherwise). 2011 ignores 192 (sub_8239D750 skips it) and plays 193 as a sound / voice
    cue (u16 id): a 193 is kept only for a cue id in `cues`, the ones 2011's own match copies play (Game11.cue_ids)
    - another id is the source's own sound. Jumps (ops 49 / 50) to the move -> its new id"""
    out = []
    for e in evs:
        if e and e[0] in (192, 193) and e[0] not in keep:
            continue
        if e and e[0] == 193 and cues is not None and (len(e) < 3 or struct.unpack_from('<H', e, 1)[0] not in cues):
            continue
        e = bytearray(e)
        if e and e[0] in (49, 50) and len(e) >= 3 and struct.unpack_from('<H', e, 1)[0] == old:
            struct.pack_into('<H', e, 1, new)
        out.append(bytes(e))
    return out


def convert0789(a, rep, outdir, g):
    import ymks
    mid = int(a['item'])
    idx = g.index()
    if mid not in idx:
        raise BadInput("%s has no motion for move %d on a YMKs bank" % (a['game'], mid))
    name = (a.get('name') or g.waze.get(mid) or 'MOVE %d' % mid)[:63]
    path0 = idx[mid][min(idx[mid])][1]
    b8 = g.bank(path0)
    tracks = [e for e in b8.entries if e['id'] == mid]
    e00 = next((e for e in tracks if e['x'] == 0 and e['y'] == 0), tracks[0])
    nf = e00['frames']
    rep['notes'].append('source bank %s: %d tracks, %d frames' % (path0, len(tracks), nf))
    e01 = next((e for e in tracks if e['x'] == 0 and e['y'] == 1), None)
    gap = None
    if e01 is not None:
        try:
            gap = start_gap(ymks.body_keys(e00['data'], b8.T), ymks.body_keys(e01['data'], b8.T))
        except Exception:   # noqa: BLE001
            gap = None
    progress(25, '2011 banks')
    g11 = a.get('_g11') or Game11(a['pac11'], os.path.join(outdir, '.work_cache'))
    tid, how = (int(a['template']), 'option') if a.get('template') else (None, '')
    if tid and not usable11(g11.waze, tid):
        rep['warnings'].append("template %d is a move the game can't play (no move record): picked another" % tid)
        tid = None
    paired = e01 is not None
    holds = sorted({e['x'] for e in tracks if 100 <= e['x'] < 110})
    # Where the move starts (WAZE +0x73: 0 standing, 2 ..., 9 a dive, 10 the corner, 16 running): 2011's record for the
    # id when it keeps one (cut moves keep theirs), else the source's - the same field, the same values. A template
    # from another situation started the move in the wrong place (Frog Splash Pin on a running move's record).
    sit = g11.situation.get(mid) if mid in g11.waze else g.situation.get(mid)
    # The templates a match can play the move from: a keyframe (YMKs) match copy in MOTP/GAME - Ankle Lock 1 on
    # Powerful Knee Strike 5090 (no YMKs match copy) got a menu bank only and never played in a match
    motp = {k[0] for p, (kind, keys, size) in g11.idx.items() if p.startswith('MOTP') and kind == 'YMKs' for k in keys}
    held = {k[0] for p, (kind, keys, size) in g11.idx.items() if kind == 'YMKs' for k in keys if k[1] in (101, 102, 103)}
    # (template categories: the slot's own (moveset_port passes its pro_slots bank), else the source's own menu
    # category (2009's STND / DIVI / ROPE ...), else every category of a one-on-one match - a tag team, Royal Rumble,
    # ladder or cell move as the template made a singles move a team move: Sabu's Arabian Facebuster 6861 got 2647,
    # MVMT/TEAM)
    cats = slot_categories(a.get('cat'))
    src_cats = [] if a.get('cat') else g.menu_categories(mid)
    if tid and tid not in motp:
        raise BadInput('template %d has no keyframe match copy (MOTP/GAME): the move would never play in a match' % tid)
    if not tid:
        # by situation and category first, the start gap and length second; for a hold (the source's x 101-103: hold
        # loop / release) a keyframe hold of 2011's (Ankle Lock 2 6181, Cloverleaf Pain 6212 ...) - a strike or a
        # roll-up gave the hold no loop
        def pool(same_sit, hold):
            return {i for i in motp if (not same_sit or sit is None or g11.situation.get(i) == sit)
                    and (not hold or i in held)}
        tries = []
        for same_sit, hold in ((True, bool(holds)), (True, False), (False, bool(holds)), (False, False)):
            for cs, where in ((src_cats, 'the source\'s categories %s' % '/'.join(src_cats)),
                              (cats, 'the slot\'s categories %s' % '/'.join(cats)), (SINGLES, 'one-on-one categories')):
                if cs:
                    tries.append((cs, where, same_sit, hold))
        # (a dive (situation 9) in the ring: not 2011's dive to the floor, Leap of Faith 3591 (flights x 110 / 111) -
        # 2007-2009 give every top-rope dive floor flights for an opponent outside, 2011 has that one move for it; as
        # a template its EXH made 2008's Swanton Bomb 1 a finisher triggered running, not from the top rope - report
        # 20261101-060824. The floor flights are then dropped below. A dive with no in-ring template still gets one.)
        dive = sit == 9
        # (2011's moves named after the source - its name and more words: SvR 2008's The 619 -> The 619 Senton 6604 /
        # The 619 Body Splash 6605, rope moves. By start gap alone it got Spinning Toe Hold 4576, a standing grapple's
        # record and menus: it wasn't where a player looks for it in CREATE A MOVE-SET - report 20261102-170057)
        words = lambda s: re.findall(r'[a-z0-9]+', s.lower())
        src_w = words(name)
        family = {i for i in motp if i in g11.waze and len(src_w) >= 2 and usable11(g11.waze, i)
                  and words(g11.waze[i][1])[:len(src_w)] == src_w and len(words(g11.waze[i][1])) > len(src_w)
                  and (sit is None or g11.situation.get(i) == sit)}
        if family:
            tid = choose_template(g11, SINGLES, nf, gap, family, paired)
            if tid:
                how = 'the 2011 moves named "%s ...", the closest start gap and length' % name
        # (a signature / finisher - moveset_port's special slots: 2011's own signatures / finishers of the move's
        # situation first - SvR 2008's Rated RKO, a corner finisher, got Corner Forehand Chop 5728, a corner strike's
        # record and menus, report 20261104-071454; 2011 has corner finishers - Twist of Fate 2 5706 ...)
        if not tid and a.get('special'):
            special = {i for i in g11.special_ids() if i in motp and usable11(g11.waze, i)
                       and (sit is None or g11.situation.get(i) == sit) and (not holds or i in held)}
            if special:
                tid = choose_template(g11, SINGLES, nf, gap, special, paired)
                if tid:
                    how = '2011\'s signatures / finishers%s, the closest start gap and length' % (
                        ', situation %s' % sit if sit is not None else '')
        for cs, where, same_sit, hold in ([] if tid else tries):
            tid = (dive and choose_template(g11, cs, nf, gap, pool(same_sit, hold), paired, ringside=False)) or \
                choose_template(g11, cs, nf, gap, pool(same_sit, hold), paired)
            if tid:
                how = '%s%s%s, the closest start gap and length (%s)' % (
                    'two-person moves' if paired else 'one-person moves (a strike, a dive)',
                    ', situation %s' % sit if same_sit and sit is not None else '',
                    ', keyframe holds (x 101-103)' if hold else '', where)
                break
    if not tid or tid not in g11.waze:
        raise BadInput('no 2011 template move with a keyframe match copy; give --template')
    # A hold with no keyframe hold of its own situation (2011's three - 6019, 6181, 6212 - are ground holds; SvR 2009's
    # Ankle Lock 1 starts standing): banks and records from the hold (in-game its x 101 loop then holds the ankle -
    # on a standing grapple's banks the pair let go after the takedown), the category bits from the template of the
    # move's situation (offered standing, as the source)
    btid = tid
    if holds and tid not in held and not a.get('template'):
        btid = choose_template(g11, SINGLES, nf, gap, pool(False, True), paired) or tid
        if btid != tid:
            rep['notes'].append('banks and records from the keyframe hold %d %s (x 101-103); category bits from %d %s'
                                % (btid, g11.waze[btid][1], tid, g11.waze[tid][1]))
    if a.get('id'):
        new_id, why = int(a['id']), 'option'
    elif mid in g11.waze and mid not in g11.moved and mid > 1000:
        new_id, why = mid, 'the same id: a record 2011 keeps without a motion'
    else:
        new_id, why = g11.free_record(name, limit=g11.id_limit(btid))
    # (an id the game never starts - 16000 and up outside the template's thousand (move_packs.cpp NeverStarts: the
    # move isn't usable, movesets keep the base's) - or past its banks' last id: another free record. moveset_port's
    # pool gave SvR 2009's Hunting RKO 16126 on Viper RKO 6863's banks - in CREATE A MOVE-SET, never in a match,
    # report 20261104-072336)
    lim = g11.id_limit(btid)
    own = new_id == mid and why.startswith('the same id')   # (the move's own cut record: kept unless the game can't use it)
    if new_id and ((new_id >= 16000 and new_id // 1000 != btid // 1000) or (lim is not None and new_id > lim) or
                   (not own and new_id not in g11.free_ids(lim))):
        old = new_id
        new_id, why = g11.free_record(name, limit=lim, avoid=a.get('_avoid'))
        rep['warnings'].append('id %d is one the game never starts with template %d: %s instead' % (old, btid, new_id))
    if not new_id:
        raise RuntimeError('no free 2011 move record')
    rep['notes'].append('template %d %s (%s)' % (tid, g11.waze[tid][1], how))
    rep['notes'].append('2011 id %d (%s)' % (new_id, why))
    pack = re.sub(r'[^a-z0-9]+', '_', name.lower()).strip('_')[:40] or 'move_%d' % mid
    pd = os.path.join(outdir, pack)
    os.makedirs(os.path.join(pd, 'motions'), exist_ok=True)
    # The record: on the id's own 2011 record (a cut move keeps its situation, damage and ratings - only its category
    # bits are empty) the template's category bits alone; elsewhere the template's whole record
    if new_id == mid and mid in g11.waze and not any(g11.waze[mid][0][:15]):
        lines = ['waze %d %s' % (new_id, bytes(g11.waze[tid][0]).hex())]
        rep['notes'].append('record: 2011\'s own for %d (situation %s), the template\'s category bits' % (new_id, sit))
    else:
        lines = ['wazecopy %d %d' % (new_id, btid)]
        if btid != tid:
            lines.append('waze %d %s' % (new_id, bytes(g11.waze[tid][0]).hex()))
        if sit is not None and g11.situation.get(btid) != sit:
            rep['warnings'].append('the template starts in situation %s, the move in %s' % (g11.situation.get(btid), sit))
    lines.append('wazename %d %s' % (new_id, name))
    progress(50, 'tracks')
    # Every track of the move goes into every bank (cameras y 50-57, ropes / turnbuckles 10-21, parts 30-32, props 60,
    # 70), the match copy too - as stock 2011 keeps them (867 of its 988 moves with a camera have it in MOTP/GAME). Only
    # the people went into MOTP before: Chokeslam 5 played without its rope and turnbuckle tracks in a match.
    pk_path = g.packed_bank(mid)
    pk = g.bank(pk_path) if pk_path else None
    pk_tracks = {(e['x'], e['y']): e for e in pk.entries if e['id'] == mid} if pk else {}
    dlen = None
    nmot = 0
    motp_out = 0
    # (an in-ring dive drops the source's floor-dive flights x 110 / 111: 2011 keeps those for a dive to the floor only
    # (Leap of Faith 3591; its in-ring dives 3555 / 3568 have x 50 and 112) and plays x 110 as the miss - SvR 2009's
    # Frog Splash Pin flew out of the ring onto the floor on a miss)
    tx = {x for p, kd, ks in g11.banks_of(btid) for x, y in ks}
    floor = {e['x'] for e in tracks if e['x'] in (110, 111)} - tx if not tx & {110, 111} else set()
    if floor:
        # (and the source's take-off, x 0: 2007-2009's short one (Swanton Bomb 1: 9 frames) ends on a branch to
        # its floor flight - without it the dive stayed crouched on the turnbuckle; the template's take-off (3550:
        # 32 frames) goes on to its hit x 30 / 40 or miss x 50 - added below)
        tracks = [e for e in tracks if e['x'] not in floor and e['x'] != 0]
        rep['dropped'].append('the floor-dive flights x %s and the take-off x 0 (the template is an in-ring dive: its own)'
                              % ' / '.join(map(str, sorted(floor))))
    for path, kind, ks in g11.banks_of(btid):
        if kind != 'YMKs':
            rep['dropped'].append('bank %s (%s)' % (path, kind))
            continue
        pac, bank = g11.bank(path)
        if dlen is None:
            d0 = bank.find(btid, 0, 0)
            dlen = d0['frames'] if d0 else None
        packed = path.startswith('MOTP')
        keep = {op for op in (192, 193) if b8.T[op] == bank.T[op] and bank.T[op]}
        cues = g11.cue_ids()
        for e in tracks:
            x, y = e['x'], e['y']
            # (menu banks: the body tracks only - of every phase of the move: SvR 2008's Rated RKO keeps the move in x 5
            # after a 13-frame x 0, and its menu bank had x 0 alone - report 20261104-071454)
            if path.startswith('MVMT') and not (y in (0, 1) and x < 90) and not any(xx == x for xx, yy in ks):
                continue
            p = pk_tracks.get((x, y)) if packed else None
            if p is not None:   # (SvR 2009's own match copy, as it is: the same packed codec)
                data = fix_track(p['data'], pk.T, y in (0, 1) and ymks.is_packed_motion(p['data'], pk.T), mid, new_id,
                                 keep, cues)
                frames = p['frames']
            else:
                keys = None
                if y in (0, 1, 2):
                    try:
                        keys = ymks.body_keys(e['data'], b8.T, False)
                    except Exception:   # noqa: BLE001
                        keys = None
                if keys is not None:
                    for k in keys:
                        k['events'] = clean_events08(k['events'], mid, new_id, keep, cues)
                        if not k.get('has_pose', True):
                            k['root'] = k['bones'] = None
                    data = ymks.build_motion(keys, max(1, e['frames']), packed=packed and y in (0, 1))
                else:   # (cameras, ropes, props: the unpacked conventions in MOTP too - ymks.is_packed_motion)
                    data = fix_track(e['data'], b8.T, False, mid, new_id, keep, cues)
                frames = e['frames']
            fn = 'motions/%d_%d_%d_%s.ymk' % (new_id, x, y, path.replace('/', '').lower())
            open(os.path.join(pd, fn), 'wb').write(data)
            lines.append('motion %s %s %d %d %d %d %s' % (pac, path, new_id, x, y, frames, fn))
            nmot += 1
            motp_out += packed
        # (with its floor flights left out the dive needs the template's miss, x 50: without one it hung in the air
        # at the corner after the flight)
        for x, y in ks:
            te = bank.find(btid, x, y) if floor and x in (0, 50) and not any(e['x'] == x for e in tracks) else None
            if te is None:
                continue
            data = fix_track(te['data'], bank.T, packed and y in (0, 1) and ymks.is_packed_motion(te['data'], bank.T),
                             btid, new_id, (192, 193))
            fn = 'motions/%d_%d_%d_%s.ymk' % (new_id, x, y, path.replace('/', '').lower())
            open(os.path.join(pd, fn), 'wb').write(data)
            lines.append('motion %s %s %d %d %d %d %s' % (pac, path, new_id, x, y, te['frames'], fn))
            nmot += 1
            if not any('template\'s miss' in n for n in rep['notes']):
                rep['notes'].append('the template\'s miss (x 50) for the dropped floor flights')
    if not motp_out:   # (never a pack without the match copy: a move with menu banks only never plays in a match)
        raise RuntimeError('no match copy (MOTP/GAME) written for template %d' % btid)
    scale = nf / float(dlen or nf)
    for kind, grp, r, ev in g11.records(btid):
        r = bytearray(r)
        struct.pack_into('<H', r, 0, new_id)
        if kind == 'evt':   # (sound frames to the new length)
            ev2 = bytearray(ev)
            for o in range(0, len(ev2) - 3, 4):
                sid, fr = struct.unpack_from('<Hh', ev2, o)
                struct.pack_into('<Hh', ev2, o, sid, int(round(fr * scale)))
            lines.append('evt %d %s %s' % (grp, bytes(r).hex(), bytes(ev2).hex()))
        else:
            lines.append('%s %d %s' % (kind, grp, bytes(r).hex()))
    open(os.path.join(pd, 'pack.txt'), 'w', newline='\n').write('\n'.join(lines) + '\n')
    rep['notes'].append('%d motions (%d in the match copy%s), the source\'s tracks copied (%d frames; template %s)' % (
        nmot, motp_out, ', SvR 2009\'s own packed copy %s' % pk_path if pk else '', nf, dlen))
    rep['dropped'].append('the source\'s own sounds and damage (the template\'s records, sounds retimed)')
    rep['match_copy'] = True
    rep.update(ok=True, out=pd, folder=pd, id=new_id, name=name, template=btid)
    progress(100, 'done')
    return rep


def retime_camera(data, scale, nframes, old, new):
    """a camera track (y 50-57: unpacked records - a camera key, count + pose, after an optional event block) stretched
    in time by `scale` to `nframes`: each key's frame and its count (the frames it takes to reach that shot) scaled,
    its events moved with it, jumps to the template pointed at the new move"""
    import ymks
    out = []
    last = -1
    for f, r in ymks.parse_stream(data).records():
        ev, pose = ymks.split_record(r, False)
        try:   # (a key size ymks doesn't read - 11 bytes in some tracks - keeps its bytes, only moved in time)
            cam = ymks.decode_camera(pose) if pose else None
        except ValueError:
            cam = None
        if cam:
            cam['count'] = max(0, min(255, int(round((cam['count'] + 1) * scale)) - 1))
            pose = ymks.encode_camera(cam)
        if ev and old != new:
            e2 = bytearray(ev)
            if e2[0] in (49, 50) and len(e2) >= 3 and struct.unpack_from('<H', e2, 1)[0] == old:
                struct.pack_into('<H', e2, 1, new)
            ev = bytes(e2)
        nf = max(last + 1, min(nframes - 1, int(round(f * scale))))
        last = nf
        out.append((nf, ymks.join_record(ev, pose, False)))
    return ymks.build_stream(ymks.make_stream(out, max(nframes, last + 1)))


def fix_track(data, T, packed, old, new, keep, cues=None):
    """a track copied as it is (cameras, ropes, props; SvR 2009's packed people): its events as clean_events08"""
    import ymks
    st = ymks.parse_stream(data)
    items, changed = [], False
    for it in st.items:
        if it[0] == 'rec':
            ev, pose = ymks.split_record(it[1], packed)
            if ev:
                lst, ok = ymks.parse_events(ev, T)
                if ok:
                    ev2 = b''.join(clean_events08([b for op, b in lst], old, new, keep, cues))
                    if ev2 != ev:
                        r = ymks.join_record(ev2, pose, packed)
                        it, changed = ('rec', r, len(r) > 0x9f), True
        items.append(it)
    return ymks.build_stream(ymks.Stream(items)) if changed else data


def waze_records(raw):
    """{move id: name} from a MOVS/WAZE table: 160-byte records (name +0x10, id +0x90) on a grid whose
    start differs by game ('12 0x430): the start where the most records hold a readable name"""
    def score(s):
        ok = 0
        for k in range(64):
            nm = raw[s + 160 * k + 0x10: s + 160 * k + 0x30].split(b'\0')[0]
            ok += bool(nm) and all(32 <= c < 127 for c in nm)
        return ok
    start = max(range(0x10, 0x1000, 8), key=score)
    out = {}
    for s in range(start, len(raw) - 159, 160):
        r = raw[s:s + 160]
        if not r.strip(b'\0'):
            continue
        nm = r[0x10:0x50].split(b'\0')[0]
        if nm and all(32 <= c < 127 for c in nm):
            out.setdefault(struct.unpack_from('<H', r, 0x90)[0], nm.decode('latin1').strip())
    return out


def waze_records_any(raw):
    """a package's move table: as waze_records, or 2K16's: a PACH whose entry 20 has 96-byte records (id +0, name +0x20)"""
    if raw[:4] != b'PACH':
        return waze_records(raw)
    out = {}
    tab = dict(svrfmt.pach_read(raw)).get(20, b'')
    for q in range(0, len(tab) - 95, 96):
        nm = tab[q + 0x20:q + 0x60].split(b'\0')[0]
        if nm and all(32 <= c < 127 for c in nm):
            out.setdefault(struct.unpack_from('<H', tab, q)[0], nm.decode('latin1').strip())
    return out


# --------------------------------------------------------------------------- SvR 2011

def usable11(waze, mid):
    """a 2011 move the game can play (as its MoveUsable): a misc.pac WAZE record with move-kind bits. Stock
    profiles hold a few it can't (RVD's 10280 and 35781 have no record): never a template or a pick."""
    r = waze.get(mid)
    return bool(r and any(r[0]))


_DLC_IDS = None


def dlc_move_ids():
    """SvR 2011's DLC move ids (dlc_move_ids.json next to this file) - never a ported move's record"""
    global _DLC_IDS
    if _DLC_IDS is None:
        try:
            _DLC_IDS = frozenset(json.load(open(os.path.join(HERE, 'dlc_move_ids.json')))['ids'])
        except (OSError, ValueError, KeyError):
            _DLC_IDS = frozenset()
    return _DLC_IDS


class Game11:
    def __init__(self, pac11, work):
        import svr10_moves as SM
        import svr10_superstar as SS
        if not os.path.exists(os.path.join(pac11, 'misc.pac')):
            raise BadInput('--pac11 is not an SvR 2011 pac folder: %s' % pac11)
        self.pac11, self.SM = pac11, SM
        self.waze = SS.Game(pac11, 11).waze()          # {id: (bits, name)}
        self.situation = SS.Game(pac11, 11).situation()  # {id: where it starts - WAZE +0x73}
        os.makedirs(work, exist_ok=True)
        self.idx = SM.bank_index(pac11, os.path.join(work, 'bank_index11.pkl'))
        self.moved = set()
        for path, (kind, keys, size) in self.idx.items():
            for k in keys:
                self.moved.add(k[0])
        self._g = {}
        self._raw = {}
        self._by_mid = None
        self._gaps, self._gaps_dirty = None, False
        # (shared between converts - a superstar convert's work folder is new each time; per 2011 pac folder)
        self._gaps_file = os.path.join(cache_dir(pac11, work),
                                       'template_gaps11_%08x.pkl' % zlib.crc32(os.path.abspath(pac11).lower().encode()))

    def groups(self, pac):
        if pac not in self._g:
            self._g[pac] = self.SM.read_epac(os.path.join(self.pac11, pac))[1]
        return self._g[pac]

    def bank(self, path):
        import ymks
        pac, raw = self.bank_raw(path)
        return pac, ymks.read_bank(raw)

    def bank_raw(self, path):
        """(pac, the bank's unpacked bytes) - YMBs banks, which ymks doesn't read; kept (a moveset port
        reads the same template banks for every move)"""
        pac = 'mpsp.pac' if path.startswith('MOTP') else 'm.pac'
        if path not in self._raw:
            parts = path.split('/')
            blob = self.SM.entry_get(self.groups(pac), parts[0].ljust(4).encode(), parts[1].ljust(4).encode())
            if len(parts) > 2:
                blob = self.SM.tree_get(blob, [int(x, 16) for x in parts[2:]])
            self._raw[path] = self.SM.unpack(blob)
        return pac, self._raw[path]

    def template_gap(self, path, bank, e):
        """start_gap of a 2011 template's attacker / victim motions - kept, and saved in the work folder
        (choose_template looks at every template of a category: decoding them all took ~25 s a move)"""
        import pickle
        import ymks
        if self._gaps is None:
            m = os.path.join(self.pac11, 'm.pac')
            self._gaps_stamp = (os.path.getsize(m), int(os.path.getmtime(m)))
            try:
                stamp, self._gaps = pickle.load(open(self._gaps_file, 'rb'))
                if stamp != self._gaps_stamp:
                    self._gaps = {}
            except Exception:   # noqa: BLE001
                self._gaps = {}
        k = (path, e['id'])
        if k not in self._gaps:
            v = bank.find(e['id'], 0, 1)
            self._gaps[k] = start_gap(ymks.body_keys(e['data'], bank.T), ymks.body_keys(v['data'], bank.T)) if v else None
            self._gaps_dirty = True
        return self._gaps[k]

    def template_attacker_end_y(self, path, bank, e):
        """the height (root y) a 2011 template's attacker ends at - a dive onto a lying victim ends low, on him
        (a dive's x 50 y 1, the victim's branch, is the miss: standing - no help); kept with the start gaps"""
        import ymks
        if self._gaps is None:
            self.template_gap(path, bank, e)
        k = (path, e['id'], 'attacker end y')
        if k not in self._gaps:
            r = [kk['root'] for kk in ymks.body_keys(e['data'], bank.T) if kk.get('root')]
            self._gaps[k] = r[-1][1] if r else None
            self._gaps_dirty = True
        return self._gaps[k]

    def cue_ids(self):
        """the sound / voice cue ids (event op 193) 2011's own match copies (MOTP/GAME) play - kept in a file per
        2011 pac folder, as the start gaps"""
        import pickle
        import ymks
        if getattr(self, '_cues', None) is not None:
            return self._cues
        m = os.path.join(self.pac11, 'mpsp.pac')
        stamp = (os.path.getsize(m), int(os.path.getmtime(m)))
        f = self._gaps_file.replace('template_gaps11_', 'cue_ids11_')
        try:
            st, self._cues = pickle.load(open(f, 'rb'))
            if st == stamp:
                return self._cues
        except Exception:   # noqa: BLE001
            pass
        self._cues = set()
        for path, (kind, keys, size) in self.idx.items():
            if kind != 'YMKs' or not path.startswith('MOTP'):
                continue
            pac, bank = self.bank(path)
            for e in bank.entries:
                packed = e['y'] in (0, 1) and ymks.is_packed_motion(e['data'], bank.T)
                try:
                    for fr, r in ymks.parse_stream(e['data']).records():
                        ev, pose = ymks.split_record(r, bool(packed))
                        for op, b in (ymks.parse_events(ev, bank.T)[0] if ev else []):
                            if op == 193 and len(b) >= 3:
                                self._cues.add(struct.unpack_from('<H', b, 1)[0])
                except ValueError:
                    pass
        try:
            save_atomic(f, obj=(stamp, self._cues))
        except OSError:
            pass
        return self._cues

    def hold_heights(self, tid):
        """(start, hold) of a 2011 YMBs submission's victim: its root height (model units) at x 0's first frame and the
        median through x 2 (the struggle loop - in the hold); kept with the start gaps. None if unread"""
        import statistics
        import ymbs
        import ymbs_retarget as YR
        if self._gaps is None:
            self._gaps_stamp = None
            m = os.path.join(self.pac11, 'm.pac')
            self._gaps_stamp = (os.path.getsize(m), int(os.path.getmtime(m)))
            try:
                stamp, self._gaps = __import__('pickle').load(open(self._gaps_file, 'rb'))
                if stamp != self._gaps_stamp:
                    self._gaps = {}
            except Exception:   # noqa: BLE001
                self._gaps = {}
        k = ('hold heights', tid)
        if k not in self._gaps:
            out = None
            for path, kind, ks in self.banks_of(tid):
                if kind != 'YMBs' or (0, 1) not in ks or (2, 1) not in ks:
                    continue
                try:
                    b = ymbs.Bank(self.bank_raw(path)[1])
                    k0, k2 = b.find(tid, 0, 1), b.find(tid, 2, 1)
                    d0, d2 = b.decode(k0)[0], b.decode(k2)[0]
                    n2 = max(1, b.dir[k2][4])
                    out = (float(YR.frame_values(d0, 0)['root_t'][1]),
                           float(statistics.median(YR.frame_values(d2, f)['root_t'][1] for f in range(0, n2, max(1, n2 // 8)))))
                except (IndexError, KeyError, ValueError, TypeError):
                    out = None
                break
            self._gaps[k] = out
            self._gaps_dirty = True
        return self._gaps[k]

    def save_gaps(self):
        import pickle
        if self._gaps_dirty:
            try:
                save_atomic(self._gaps_file, obj=(self._gaps_stamp, self._gaps))
            except OSError:
                pass
            self._gaps_dirty = False

    def banks_of(self, mid):
        """[(path, kind, [(x, y)])]"""
        if self._by_mid is None:
            by = {}
            for path, (kind, keys, size) in self.idx.items():
                for k in keys:
                    by.setdefault(k[0], {}).setdefault(path, set()).add((k[1], k[2]))
            self._by_mid = {m: [(p, self.idx[p][0], sorted(xs)) for p, xs in d.items()] for m, d in by.items()}
        return list(self._by_mid.get(mid, []))

    def records(self, mid):
        """the move's WAZA/DATA records: [(kind, group, record bytes, events bytes)]"""
        SM = self.SM
        kids = SM.waza_children(self.groups('misc.pac'))
        out = []
        for i, g_id, raw in SM.subgroups(kids[0]):
            for r in SM.exh_parse(raw, 36):
                if struct.unpack_from('<H', r, 0)[0] == mid:
                    out.append(('exh', i, r, b''))
        for i, g_id, raw in SM.subgroups(kids[1]):
            pr = SM.evt_parse(raw)
            if not pr:
                continue
            recs, ev = pr
            for r in recs:
                if struct.unpack_from('<H', r, 0)[0] == mid:
                    out.append(('evt', i, r, b''.join(SM.evt_list(r, ev))))
        for i, g_id, raw in SM.subgroups(kids[2]):
            for k in range(len(raw) // 8):
                r = raw[8 * k:8 * k + 8]
                if struct.unpack_from('<H', r, 0)[0] == mid:
                    out.append(('mbd', i, r, b''))
        return out

    def in_profiles(self):
        """the move ids 2011's own profiles use (a record one of them uses is never free: rewriting it would
        change that superstar's move)"""
        if getattr(self, '_in_pro', None) is None:
            import svr10_superstar as SS
            self._in_pro = set()
            for r in SS.Game(self.pac11, 11).pro.values():
                self._in_pro.update(struct.unpack_from('<%dH' % (min(len(r), 0x1C0) // 2), r, 0))
        return self._in_pro

    def id_limit(self, tid):
        """the highest id a move on template `tid`'s banks can have: each bank's directory is sorted and ends at
        its highest id, and the game's merge only inserts below it (move_packs.cpp BankInsert) - 2K15's ported AA
        on record 17013 went into no bank (MOT/BM00/0 ends in the 5000s): the move never started"""
        tops = [max(k[0] for k in self.idx[p][1]) for p, kind, xs in self.banks_of(tid) if self.idx[p][1]]
        return min(tops) - 1 if tops else None

    def special_ids(self):
        """the moves 2011's own profiles have as signatures / finishers (slots 0x1E-0x24)"""
        if getattr(self, '_special', None) is None:
            import svr10_superstar as SS
            self._special = set()
            for r in SS.Game(self.pac11, 11).pro.values():
                if len(r) >= 0x26:
                    self._special.update(struct.unpack_from('<4H', r, 0x1E))
        return self._special

    def free_ids(self, limit=None, avoid=(), near=None):
        """the free move records, sorted - the one rule for every picker (free_record, moveset_port, the Mod Doctor's
        renumber): a record 2011 keeps without motions (a cut move), above 1000, in no 2011 profile, not in `avoid`,
        at most `limit` (id_limit). near: a template id - only records in its thousand when there are any (a taunt in
        9000-9999: an in-match taunt on a record elsewhere, 3894, wasn't found - Sabu's 9040 was); without one no
        9000s (the game's streamed (YMBs) motions are 9000-9779 by its flag table: a keyframe move there played as one)"""
        # (nor a DLC move: 2011's DLC brings 282 records the disc keeps cut back to life, with its own motions, and adds
        # 18 - dlc_move_ids.json. A ported move on one collided with the DLC's own for every player with the DLC: 273
        # of the "free" records were such - Brock Lesnar 2K14 / 2K15 ports, reports 20261104-091730 / -093125)
        dlc = dlc_move_ids()
        free = [i for i in sorted(self.waze) if i not in self.moved and i > 1000 and i not in self.in_profiles()
                and (limit is None or i <= limit) and i not in avoid and i not in dlc]
        # (not 16000 and up outside the template's own thousand: a move there never starts - the game doesn't even
        # ask for its motion (move_packs.cpp NeverStarts). WWE '12's Octopus Stretch on 17071 dropped both people to
        # stance; on 5198 it played. 2K15's AA on 17013, Mankind '13's Mandible Claw on 17038, SvR 2009's Hunting RKO
        # on 16126 - report 20261104-072336.)
        free = [i for i in free if i < 16000 or (near is not None and i // 1000 == near // 1000)]
        if near is not None and any(i // 1000 == near // 1000 for i in free):
            return [i for i in free if i // 1000 == near // 1000]
        return [i for i in free if i // 1000 != 9] if near is None else free

    def free_record(self, name=None, near=None, limit=None, avoid=None):
        """a record 2011 keeps without motions (a cut move) - free_ids: the one named `name`, else one picked by the
        name. limit: the highest id that fits (id_limit)"""
        free = self.free_ids(limit, avoid or (), near)
        if name:
            for i in free:
                if norm(self.waze[i][1]) == norm(name):
                    return i, 'the free record with the same name'
        if not free:
            return None, ''
        # (another converted move should get another record: picked by the name, not the first free one;
        # the Mod Maker passes --id to keep clear of the packs already installed)
        h = zlib.crc32((name or '').encode('utf-8'))
        return free[h % len(free)], 'a free record picked by the name (%d free; --id to choose)' % len(free)


# --------------------------------------------------------------------------- renumber

def read_lines(path):
    """(lines without their ends, crlf)"""
    raw = open(path, 'rb').read()
    return raw.decode('utf-8').replace('\r\n', '\n').split('\n'), b'\r\n' in raw


def write_lines(path, lines, crlf):
    text = '\n'.join(lines)
    open(path, 'wb').write((text.replace('\n', '\r\n') if crlf else text).encode('utf-8'))


def pack_defines(lines):
    """the move ids a pack.txt's lines give: {id: dict(paths, ymbs, name)} - motion, waze, wazecopy, wazename"""
    out = {}
    blank = lambda: dict(paths=set(), ymbs=False, name='')   # noqa: E731
    for l in lines:
        f = l.split()
        if len(f) < 2 or f[0].startswith('#'):
            continue
        if f[0] == 'motion' and len(f) >= 8 and f[3].isdigit():
            d = out.setdefault(int(f[3]), blank())
            d['paths'].add(f[2])
            d['ymbs'] |= len(f) >= 9 and len(f[8]) == 40   # (a 20-byte YMBs header: a streamed motion)
        elif f[0] in ('waze', 'wazecopy') and f[1].isdigit():
            out.setdefault(int(f[1]), blank())
        elif f[0] == 'wazename' and f[1].isdigit() and len(f) >= 3:
            out.setdefault(int(f[1]), blank())['name'] = l.split(None, 2)[2].strip()
    return out


def pack_dir(mod):
    for d in (os.path.join(mod, 'moves'), mod):
        if os.path.isfile(os.path.join(d, 'pack.txt')):
            return d
    raise BadInput('no moves/pack.txt or pack.txt in %s' % mod)


def manifest_get(mod, key, default=''):
    try:
        for l in read_lines(os.path.join(mod, 'manifest.txt'))[0]:
            if l.startswith(key + '='):
                return l[len(key) + 1:].strip()
    except OSError:
        pass
    return default


def renumber(a):
    """renumber --mod: see the top of the file. -> the report"""
    import svr10_moves as SM
    mod = os.path.abspath(a['mod'])
    pdir = pack_dir(mod)
    lines, crlf = read_lines(os.path.join(pdir, 'pack.txt'))
    defs = pack_defines(lines)
    g11 = Game11(a['pac11'], a.get('work') or os.path.join(HERE, 'work', 'renumber'))
    mods_root = os.path.abspath(a.get('mods') or os.path.join(os.path.dirname(os.path.abspath(a['pac11'])), 'Mods'))
    # the ids every other installed pack gives (a new id must be none of them)
    taken, others = set(defs), []
    for root, dirs, files in os.walk(mods_root):
        dirs[:] = [d for d in dirs if not d.startswith('.') and d not in ('PacOverlay', 'SuperstarOverlay')]
        if 'pack.txt' in files and os.path.abspath(root) != pdir:
            taken |= set(pack_defines(read_lines(os.path.join(root, 'pack.txt'))[0]))
    tops = {}
    for path, (kind, keys, size) in g11.idx.items():
        if keys:
            tops[path] = max(k[0] for k in keys)
    pmap, kept, rows = {}, [], []
    for old in sorted(i for i in defs if i >= 16000 and i not in g11.moved):   # (a 2011 motion's own id: the game's)
        d = defs[old]
        t = [tops[p] for p in d['paths'] if p in tops]
        limit = min([15999] + [x - 1 for x in t])   # (id_limit for a mod: its banks' last ids)
        near = 9000 if d['ymbs'] else None          # (a streamed motion: the 9000s, where the game's are)
        free = g11.free_ids(limit, taken | set(pmap.values()), near)
        name = d['name'] or (g11.waze.get(old, (None, ''))[1] if old in g11.waze else '')
        if not free:
            kept.append(dict(old=old, name=name, why='no free record below %d' % (limit + 1)))
            continue
        same = [i for i in free if name and norm(g11.waze[i][1]) == norm(name)]
        new = same[0] if same else free[zlib.crc32(('%s %d' % (name, old)).encode('utf-8')) % len(free)]
        pmap[old] = new
        rows.append(dict(old=old, new=new, name=name, limit=limit))
    rep = dict(ok=True, mod=mod, renumbered=rows, kept=kept, changed=[], backup=None, dry=bool(a.get('dry')))
    if not pmap or a.get('dry'):
        return rep
    m = lambda i: pmap.get(i, i)   # noqa: E731

    def num(tok):
        return str(m(int(tok))) if tok.isdigit() else tok
    # pack.txt
    new_lines, motion_files = [], {}
    for l in lines:
        f = l.split(' ')
        if l.startswith('# svr2011 move pack: moves'):
            head, ids = l.split('moves', 1)
            l = head + 'moves' + ' '.join(num(x) for x in ids.split(' '))
        elif f[0] == 'motion' and len(f) >= 8:
            if f[3].isdigit() and f[5].isdigit() and int(f[5]) < 50:   # (y 50+ tracks don't read as this layout)
                motion_files[f[7]] = f[2].startswith('MOTP')
            f[3] = num(f[3])
            l = ' '.join(f)
        elif f[0] in ('waze', 'wazename') and len(f) >= 2:
            f[1] = num(f[1])
            l = ' '.join(f)
        elif f[0] == 'wazecopy' and len(f) >= 3:
            f[1], f[2] = num(f[1]), num(f[2])
            l = ' '.join(f)
        elif f[0] in ('exh', 'evt', 'mbd') and len(f) >= 3 and len(f[2]) >= 4:
            rid = struct.unpack('<H', bytes.fromhex(f[2][:4]))[0]
            if rid in pmap:
                f[2] = struct.pack('<H', pmap[rid]).hex() + f[2][4:]
                l = ' '.join(f)
        new_lines.append(l)
    # the motions' follow-ups (the last op 49) naming a renumbered move
    T = SM.tree_get(SM.entry_get(g11.groups('m.pac'), *SM.path_split('MVMT/CORN/f')[:2]),
                    SM.path_split('MVMT/CORN/f')[2])[0x10:0x110]
    fixed_files = {}
    for fn, motp in sorted(motion_files.items()):
        fp = os.path.join(pdir, fn)
        try:
            data = open(fp, 'rb').read()
            at, tgt = SM.end_chain(data, T, motp)
        except (OSError, ValueError, IndexError):
            continue
        if at is not None and tgt in pmap:
            b = bytearray(data)
            struct.pack_into('<H', b, at, pmap[tgt])
            fixed_files[fp] = bytes(b)
    # the moves lists naming them: this mod's, and for a move pack every installed superstar mod's
    lists = [os.path.join(mod, manifest_get(mod, 'moves', 'moves.txt'))]
    if pdir == mod:
        stars = os.path.join(mods_root, 'Superstars')
        for d in sorted(os.listdir(stars)) if os.path.isdir(stars) else []:
            sd = os.path.join(stars, d)
            if os.path.isdir(sd) and not os.path.isfile(os.path.join(sd, 'moves', 'pack.txt')):
                lists.append(os.path.join(sd, manifest_get(sd, 'moves', 'moves.txt')))
    list_edits = {}
    for lp in lists:
        if not os.path.isfile(lp):
            continue
        ls, lcrlf = read_lines(lp)
        out = []
        for l in ls:
            k, eq, v = l.partition('=')
            out.append(k + '=' + num(v.strip()) if eq and v.strip().isdigit() and int(v.strip()) in pmap else l)
        if out != ls:
            list_edits[lp] = (out, lcrlf)
    # backup, then write
    import zipfile
    changed = ([os.path.join(pdir, 'pack.txt'), os.path.join(mod, 'manifest.txt')] + list(fixed_files) + list(list_edits) +
               [os.path.join(os.path.dirname(lp), 'manifest.txt') for lp in list_edits if os.path.dirname(lp) != mod])
    bdir = os.path.join(HERE, 'work', 'renumber_backup')
    os.makedirs(bdir, exist_ok=True)
    bzip = os.path.join(bdir, '%s_%s.zip' % (os.path.basename(mod), time.strftime('%Y%m%d-%H%M%S')))
    with zipfile.ZipFile(bzip, 'w', zipfile.ZIP_DEFLATED) as z:
        for fp in changed:
            if os.path.isfile(fp):
                z.write(fp, os.path.relpath(fp, mods_root) if fp.startswith(mods_root) else os.path.basename(fp))
    write_lines(os.path.join(pdir, 'pack.txt'), new_lines, crlf)
    for fp, data in fixed_files.items():
        open(fp, 'wb').write(data)
    for lp, (out, lcrlf) in list_edits.items():
        write_lines(lp, out, lcrlf)
    # the manifests: a new version (lobbies and the launcher see the mod changed) and renumbered=<old>><new> - a save
    # made with the old version holds the old ids, the game puts the new ones in (superstar_mods.cpp)
    for md in sorted({mod} | {os.path.dirname(lp) for lp in list_edits}):
        mp = os.path.join(md, 'manifest.txt')
        if not os.path.isfile(mp):
            continue
        man, mcrlf = read_lines(mp)
        was, at = {}, None
        for i, l in enumerate(man):
            if l.startswith('renumbered='):
                at = i
                for t in l[11:].split():
                    o, _, n = t.partition('>')
                    if o.isdigit() and n.isdigit():
                        was[int(o)] = int(n)
        was = {o: pmap.get(n, n) for o, n in was.items()}   # (renumbered again: the old ids go to the newest)
        was.update(pmap)
        line = 'renumbered=' + ' '.join('%d>%d' % kv for kv in sorted(was.items()))
        if at is None:
            while man and man[-1] == '':
                man.pop()
            man += [line, '']
        else:
            man[at] = line
        if md == mod:
            for i, l in enumerate(man):
                if l.startswith('version='):   # (its last number one up: 1.2 -> 1.3, 1.3.1 -> 1.3.2)
                    v = re.match(r'(.*?)(\d+)(\D*)$', l[8:].strip())
                    if v:
                        man[i] = 'version=%s%d%s' % (v[1], int(v[2]) + 1, v[3])
        write_lines(mp, man, mcrlf)
    rep.update(changed=[os.path.relpath(fp, mod) if fp.startswith(mod) else fp for fp in changed if os.path.isfile(fp)],
               backup=bzip)
    return rep


# --------------------------------------------------------------------------- convert

def start_gap(k0, k1):
    """the distance (x / z, root units) between two tracks' first roots"""
    r0 = next((k['root'] for k in k0 if k.get('root')), None)
    r1 = next((k['root'] for k in k1 if k.get('root')), None)
    if not r0 or not r1:
        return None
    return ((r0[0] - r1[0]) ** 2 + (r0[2] - r1[2]) ** 2) ** 0.5


# 2011's move categories (MVMT/<cat>) a one-on-one match uses - not TEAM (tag team), RUMB (Royal Rumble), LADR
# (ladder), CELL / CETB (Hell in a Cell)
SINGLES = ('ABIL', 'ANAS', 'APRO', 'BASE', 'CORN', 'FINI', 'GRND', 'ROPE', 'RUN', 'SPE1', 'SPE2', 'STAT', 'STN1', 'STN2')


def slot_categories(bank):
    """a pro_slots bank ('STN1', 'STAT', 'STND', 'ABIL+STAT', ...) -> the MVMT categories to look in first"""
    out = []
    for b in (bank or '').replace('+', ' ').split():
        b = b.strip().upper()
        out += ['STN1', 'STN2'] if b == 'STND' else [b] if b in SINGLES or b == 'RUMB' else []
    return out or list(SINGLES)


def template_travel(g11, tid):
    """(dx, dz) of a 2011 move's attacker (x 0 y 0) root from its first key to its last, None if unread"""
    import ymks
    for path, kind, ks in g11.banks_of(tid):
        if kind != 'YMKs' or (0, 0) not in ks:
            continue
        pac, bank = g11.bank(path)
        e = bank.find(tid, 0, 0)
        if e is None:
            continue
        r = [k['root'] for k in ymks.body_keys(e['data'], bank.T, path.startswith('MOTP')) if k.get('root')]
        if len(r) > 1:
            return (r[-1][0] - r[0][0], r[-1][2] - r[0][2])
    return None


def template_start(g11, tid):
    """a 2011 move's attacker (x 0 y 0) root at its first key, None if unread - the anchor the ported attacker starts on"""
    import ymks
    for path, kind, ks in g11.banks_of(tid):
        if kind != 'YMKs' or (0, 0) not in ks:
            continue
        pac, bank = g11.bank(path)
        e = bank.find(tid, 0, 0)
        if e is None:
            continue
        r = next((k['root'] for k in ymks.body_keys(e['data'], bank.T, path.startswith('MOTP')) if k.get('root')), None)
        if r:
            return r
    return None


def scale_travel(keys, k):
    """[(root, ang)] with the root's horizontal travel from the first key scaled by k (the height kept)"""
    if not keys or k == 1.0:
        return keys
    x0, z0 = keys[0][0][0], keys[0][0][2]
    return [((x0 + (r[0] - x0) * k, r[1], z0 + (r[2] - z0) * k), a) for r, a in keys]


def fall_tail(tk):
    """A 2011 dive's flight ends in a drop: the attacker's root falls on past the mat (7000 on the last key -
    Diving Double Axe Handle 3538, Frog Splash 3555) and the game lands the dive when the root reaches the mat.
    (the key the drop starts at, [root height change per key from the key before it]) - (None, []) when the
    template's flight ends in the air"""
    ks = [(k['frame'], k['root'][1]) for k in tk if k.get('root')]
    if len(ks) < 3 or ks[-1][1] < 3000:
        return None, []
    i = len(ks) - 1
    while i > 1 and ks[i - 1][1] > ks[0][1]:   # (back to where it passes its start height going down)
        i -= 1
    return ks[i][0], [y - ks[i - 1][1] for f, y in ks[i:]]


def heading_turn(st, tt, min_travel=200.0, min_angle=0.35):
    """the 4x4 turning a source about the vertical so its attacker's travel st (dx, dz) goes the template's way tt,
    and the angle; (None, 0) when either barely travels or they already agree within min_angle (radians)"""
    import math
    import numpy as np
    if not tt or math.hypot(*st) < min_travel or math.hypot(*tt) < 0.25 * min_travel:
        return None, 0.0
    a = math.atan2(tt[1], tt[0]) - math.atan2(st[1], st[0])
    a = (a + math.pi) % (2 * math.pi) - math.pi
    if abs(a) < min_angle:
        return None, 0.0
    c, s_ = math.cos(a), math.sin(a)
    M = np.eye(4)
    M[0, 0], M[0, 2], M[2, 0], M[2, 2] = c, -s_, s_, c   # (x' = c x - s z, z' = s x + c z: the angle of (x, z) + a)
    return M, a


def choose_template(g11, cat, frames, gap=None, allowed=None, paired=True, attacker_end_y=None, reversals=True,
                    with_score=False, need_x=(), ringside=True):
    """the 2011 move in MVMT/<cat> (YMKs only, x0 y0 and x0 y1 both there) most like the source:
    the attacker / victim start distance (a running move needs a running template), for a dive where the
    attacker ends (low: on a lying victim) and the length"""
    import ymks
    best = None
    cats = [c.strip() for c in cat] if isinstance(cat, (list, tuple)) else [cat.strip() if cat else '']
    for path, (kind, keys, size) in g11.idx.items():
        if not any(path.startswith('MVMT/' + c) for c in cats) or kind != 'YMKs':
            continue
        ids = {k[0] for k in keys if k[1] == 0 and k[2] == 0} & {k[0] for k in keys if k[1] == 0 and k[2] == 1}
        if not paired:   # (a one-person move - a strike, a dive: the victim reacts with its own motion - a dive's
            # reaction is a branch of its own, x 50 y 1: only x 0 counts)
            ids = {k[0] for k in keys if k[1] == 0 and k[2] == 0} - {k[0] for k in keys if k[1] == 0 and k[2] == 1}
        if not ids:
            continue
        pac, bank = g11.bank(path)
        for e in bank.entries:
            if e['id'] in ids and e['x'] == 0 and e['y'] == 0:
                if not usable11(g11.waze, e['id']):   # (wazecopy needs a record the game plays)
                    continue
                if allowed is not None and e['id'] not in allowed:
                    continue
                if not reversals and 'reversal' in g11.waze[e['id']][1].lower():
                    continue
                # (no category asked: only a move a one-on-one match has. A move only in a match type's banks - Rumble's
                # 83xx / 84xx, MOT/RU* + MVMT/RUMB - has no motions in other matches: both wrestlers stand still in it.
                # A Rumble elimination slot asks for RUMB by name and still gets them)
                if cats == [''] and not any(p.startswith('MVMT/') and p.split('/')[1].strip() in SINGLES
                                            for p, kd, ks in g11.banks_of(e['id'])):
                    continue
                # (a move from the top rope or onto an opponent seated on the turnbuckle - WAZE situation 9-11, 13,
                # 14 - only when corner moves are asked for: 2K17's Blue Thunder Bomb, a standing powerbomb, got DDT
                # 14 (23153, situation 13) through the categories its namesakes voted for)
                if 'CORN' not in cats and getattr(g11, 'situation', {}).get(e['id']) in (9, 10, 11, 13, 14):
                    continue
                # (nor one that is also a rope, apron or corner move - its events wait on the ropes: it then got
                # Dragon Special, in SPE2 and ROPE; 2K15's F-5 froze on such a template)
                if any(p.startswith('MVMT/') and p.split('/')[1] in ('ROPE', 'APRO', 'CORN') and p.split('/')[1] not in cats
                       for p, kd, ks in g11.banks_of(e['id'])):
                    continue
                if any(kd == 'YMBs' for p, kd, ks in g11.banks_of(e['id'])):
                    continue
                # (a dive's branches where the source has them: Leap of Faith keeps its miss in x 110, so a ported
                # dive judged a miss flew again with Leap of Faith's own miss flight)
                if need_x and not set(need_x) <= {x for p, kd, ks in g11.banks_of(e['id']) for x, y in ks if y == 0}:
                    continue
                # (nor, for a dive in the ring, one that flies out to the floor - Leap of Faith, flights x 110 / 111:
                # WWE '12's Springboard Moonsault, turned to its heading, flipped away from the victim out of the ring)
                if not ringside and any(x in (110, 111) for p, kd, ks in g11.banks_of(e['id']) for x, y in ks):
                    continue
                score = abs(e['frames'] - frames) / 10.0
                if gap is not None:
                    tg = g11.template_gap(path, bank, e)
                    if tg is not None:
                        score += abs(tg - gap) / 100.0
                if attacker_end_y is not None and hasattr(g11, 'template_attacker_end_y'):
                    ty = g11.template_attacker_end_y(path, bank, e)
                    score += abs(ty - attacker_end_y) / 20.0 if ty is not None else 20.0
                if best is None or score < best[0]:
                    best = (score, e['id'], path)
    if hasattr(g11, 'save_gaps'):
        g11.save_gaps()
    if with_score:
        return (best[1], best[0]) if best else (None, None)
    return best[1] if best else None


def coded_category(g, g11, cat0):
    """a coded source category (2K14+: C000, E125, ...) -> the 2011 MVMT categories its moves sit in, by the moves
    both games name alike (a vote; the categories with at least a quarter of the top one's votes). Without it any
    category was searched: 2K15's F-5 (E125) got a rope move (Throat Thrust 4) as its template - rope tracks,
    events waiting on the ropes - and froze in-game."""
    import collections
    by11 = {}
    for i, (bits, nm) in g11.waze.items():
        by11.setdefault(norm(nm), i)
    votes = collections.Counter()
    idx = g.index()
    for mid, xs in idx.items():
        if idx[mid][min(xs)][0] != cat0 or not g.waze.get(mid):
            continue
        i11 = by11.get(norm(g.waze[mid]))
        if i11 is None:
            continue
        for p, kd, ks in g11.banks_of(i11):
            c = p.split('/')[1] if p.startswith('MVMT/') else None
            if c in SINGLES or c in ('RUN ', 'TEAM'):
                votes[c] += 1
    if any(c != 'TEAM' for c in votes):   # (tag team moves only for a category of nothing else: 2K15's M090)
        votes.pop('TEAM', None)
    if not votes:
        return []
    top = votes.most_common(1)[0][1]
    return [c for c, v in votes.most_common() if v * 4 >= top]


def choose_submission(g11, frames, phases, heights=None):
    """the 2011 submission (a YMBs move in MVMT/ANAS, attacker and victim at x 0) most like a source hold: the
    most phases in common, then - heights (the source victim's root height at the start and in the hold, x 2, model
    units) - a hold as high (standing) or as low (on the mat) as the source's, then the closest length. 2K15's Kimura
    Lock goes to the mat: on Bearhug 21007 (a standing hold, its victim at -11 all through) the victim let go and stood
    while the attacker held on alone on the mat, report 20261104-093125"""
    best = None
    for path, (kind, keys, size) in g11.idx.items():
        if not path.startswith('MVMT/ANAS') or kind != 'YMBs':
            continue
        xs = {}
        for k in keys:
            xs.setdefault(k[0], set()).add(k[1])
        pairs = {k[0] for k in keys if k[1] == 0 and k[2] == 0} & {k[0] for k in keys if k[1] == 0 and k[2] == 1}
        pac, bank = g11.bank(path)
        for e in bank.entries:
            if e['id'] in pairs and e['x'] == 0 and e['y'] == 0 and usable11(g11.waze, e['id']):
                score = -10 * len(xs[e['id']] & phases) + abs(e['frames'] - frames) / 10.0
                th = g11.hold_heights(e['id']) if heights else None
                if th:   # (the hold's height first: standing holds keep the victim at -7..-11, mat holds at -1..-5)
                    score += 6.0 * abs(th[1] - heights[1]) + 3.0 * abs(th[0] - heights[0])
                if best is None or score < best[0]:
                    best = (score, e['id'])
    if hasattr(g11, 'save_gaps'):
        g11.save_gaps()
    return best[1] if best else None


def dive_kin(g11, name):
    """2011's dives (one-person MVMT/CORN moves) whose names share the source's rarest word - WWE '13's Diving
    Corkscrew Splash: Diving Corkscrew Body 3624 (the closest length gave 450 Splash, which leaves upright: the
    corkscrew starts flipped, the game dropped its base to the mat at once and it landed a torso short). An empty
    set when no name word is shared"""
    import collections
    ids = set()
    for path, (kind, keys, size) in g11.idx.items():
        if path.startswith('MVMT/CORN') and kind == 'YMKs':
            ids |= {k[0] for k in keys if k[1] == 0 and k[2] == 0} - {k[0] for k in keys if k[1] == 0 and k[2] == 1}
    ids = {i for i in ids if usable11(g11.waze, i)}

    def words(s):
        # ("star": a Shooting Star Press is no Five Star Frog Splash - WWE '12's lands on 3570)
        return set(re.findall(r'[a-z]{3,}', (s or '').lower())) - {'diving', 'pin', 'top', 'rope', 'star'}
    count = collections.Counter(w for i in ids for w in words(g11.waze[i][1]))
    # (a rare word only: "splash" names half of them - WWE '13's 450 Splash moved off 3570, the template it lands on)
    mine = [w for w in words(name) if 0 < count[w] <= 2]
    if not mine:
        return set()
    w = min(mine, key=lambda w: count[w])
    return {i for i in ids if w in words(g11.waze[i][1])}


def choose_template_by_length(g11, cat, frames):
    """(the old rule: the closest length only)"""
    best = None
    for path, (kind, keys, size) in g11.idx.items():
        if not path.startswith('MVMT/' + cat.strip()) or kind != 'YMKs':
            continue
        ids = {k[0] for k in keys if k[1] == 0 and k[2] == 0} & {k[0] for k in keys if k[1] == 0 and k[2] == 1}
        if not ids:
            continue
        pac, bank = g11.bank(path)
        for e in bank.entries:
            if e['id'] in ids and e['x'] == 0 and e['y'] == 0:
                if any(kd == 'YMBs' for p, kd, ks in g11.banks_of(e['id'])):
                    continue
                d = abs(e['frames'] - frames)
                if best is None or d < best[0]:
                    best = (d, e['id'], path)
    return best[1] if best else None


def retime(events, scale, n_len, old_id, new_id, limit=0x7f):
    """{frame: [event bytes]} moved to the new length; jumps (ops 49 / 50) to the template -> new id"""
    out = {}
    for f, evs in events:
        nf = min(n_len, int(round(f * scale)))
        for e in evs:
            e = bytearray(e)
            if e and e[0] in (49, 50) and len(e) >= 3 and struct.unpack_from('<H', e, 1)[0] == old_id:
                struct.pack_into('<H', e, 1, new_id)
            out.setdefault(nf, []).append(bytes(e))
    # (a frame holds at most 0x7f bytes of events (0x20 packed): what a shorter move piles up spills to the next frames)
    spill, res = [], {}
    for f in range(0, max(list(out) + [0]) + 64):
        cur = spill + out.get(f, [])
        spill, keep, size = [], [], 0
        for e in cur:
            if size + len(e) <= limit:
                keep.append(e)
                size += len(e)
            else:
                spill.append(e)
        if keep:
            res[f] = keep
        if not spill and f > max(list(out) + [0]):
            break
    return res


def convert(a, rep):
    import ymks
    import yanm_decode as Y
    import w13_retarget as WR
    sys.path.insert(0, HERE)
    sp = __import__('superstar_port')
    outdir = os.path.abspath(a['out'])
    os.makedirs(outdir, exist_ok=True)
    # (a['_g'] / a['_g11']: games already opened - moveset_port converts a superstar's moves in a row)
    g = a.get('_g')
    if g is None:
        src = sp.Src(a['src'], os.path.join(outdir, '.src_' + a['game']))
        progress(2, 'reading the game')
        g = open_game(a, src, os.path.join(outdir, '.work_cache'))
    if a['game'] in OLD:
        return convert0789(a, rep, outdir, g)
    try:
        mid = int(a['item'])
    except ValueError:
        raise BadInput('--item is a move id (from list)')
    idx = g.index()
    if mid not in idx:
        raise BadInput("%s has no motion for move %d" % (a['game'], mid))
    name = (a.get('name') or g.waze.get(mid) or 'MOVE %d' % mid)[:63]
    progress(10, 'decoding the motion')
    xs = sorted(idx[mid])
    # The move's records (x): one can be a short lead-in whose body keys stop at frame ~18 while its root and timing
    # tracks run the whole length - 2K14's Shell Shocked 50551: x0 5.8 KB (body to 18 of 752), x1 34 KB (to 752); the
    # first one made the body freeze in the grab pose and glide with the root. The record whose body moves longest.
    best = None
    for x in xs:
        cat_x, path_x = idx[mid][x]
        bl_x = Y.decode_record(g.record(cat_x, path_x))
        bl_x = list(bl_x.values()) if isinstance(bl_x, dict) else list(bl_x)
        body = 0
        for b in bl_x:
            for nm_, tr in b['tracks'].items():
                if not nm_.startswith('Bip '):
                    continue
                for v in tr['parts'].values():
                    ts = [k[0] for k in v] if isinstance(v, list) and v else \
                        [k[0] for k in v['keys']] if isinstance(v, dict) and v.get('keys') else []
                    if ts:
                        body = max(body, ts[-1])
        if best is None or body > best[0]:
            best = (body, x, cat_x, path_x, bl_x)
    _, x0, cat0, path0, bl = best
    if x0 != xs[0]:
        rep['notes'].append('source record x%d (its body moves the whole motion; x%d is a short lead-in)' % (x0, xs[0]))
    # blocks are named <phase 4 hex><person 4 hex>: 0000 0000/0001 a 2-frame set-up pose, 0010 0000/0001
    # the move's attacker / victim (0011.. variants), 0001-0005 0000 one-person parts. The move: the
    # first phase from 0010 with both people, else the longest one-person block.
    by = {}
    for b in bl:
        nm = str(b.get('block', '00000000')).rjust(8, '0')
        by.setdefault(nm[:4], {})[int(nm[4:], 16)] = b
    pairs = sorted(p for p, d in by.items() if 0 in d and 1 in d and d[0]['frames'] > 2)
    # A dive laid out as 2011's (WWE '12's Shooting Star Press / Frog Splash): 0000 0000 the attacker's flight alone,
    # 0030 / 0040 / 0050 the pair's branches (2011's x 30 / 40 / 50). The flight is the move - taking the first
    # pair block (0030) as the move gave a two-person template and no flight. (Taken for a corner move only, once its
    # category is known: 2K15's Springboard Crossbody, an apron move, is laid out alike.)
    flight = by['0000'][0] if ('0000' in by and 0 in by['0000'] and 1 not in by['0000'] and
                               by['0000'][0]['frames'] > 2 and any(30 <= int(p) < 90 for p in pairs if p.isdigit())) else None
    branched_dive = False
    if pairs:
        phase = pairs[0]
        att, vic = by[phase][0], by[phase][1]
        rep['notes'].append('source blocks %s0000 / %s0001 (%d frames at 60 Hz)' % (phase, phase, att['frames']))
    else:
        att = max(bl, key=lambda b: b['frames'])
        vic = None
        rep['warnings'].append('no two-person block in the source motion: the attacker only, the victim stays the template\'s')
    nf = max(1, (att['frames'] - 1) // 2)
    progress(25, '2011 banks')
    g11 = a.get('_g11') or Game11(a['pac11'], os.path.join(outdir, '.work_cache'))
    R0 = WR.Retarget(bind='rest', twist=True, ik=a['game'] != 'wwe12')
    cat11 = CATS.get(cat0, '')   # ('': every category - 2K14's coded ones)
    if cat0 == 'APR2':   # (WWE '12 / '13 moves from the apron: on an APRO template - check where it starts)
        rep['warnings'].append('an apron move in its game (APR2): in 2011 it plays as an apron move - check it in a match')
    # (a moveset slot's own category wins - moveset_port passes it: Mankind '13's Mandible Claw in a finisher
    # slot (STN1) got an apron move as its template, both people sunk in the mat; Rumble elimination slots: RUMB)
    # A signature / finisher slot is STN1 for every move, so a top-rope, turnbuckle or rope move keeps its own category:
    # WWE '12's 450 Splash 2 (CORN) in a finisher slot got Sambo Hip Toss, a front grapple, and never played in a match
    # (reports 20261107-182224, -181926). Apron and ground moves still take the slot's (the Mandible Claw above).
    own_pos = a.get('special') and CATS.get(cat0) in ('CORN', 'CETB', 'ROPE')
    if a.get('cat') and not a.get('template') and not own_pos:
        sc = slot_categories(a['cat'])
        if sc and sc != list(SINGLES):
            cat11 = sc
            rep['notes'].append("category: the moveset slot's (%s)" % '/'.join(sc))
    gap = None
    # (2K15 keeps the attacker in its own frame: turned into the victim's - w13_retarget.facing_frame)
    face = None
    if vic is not None:
        k1 = R0.keys(vic, 2)

        def gap_of(M):
            k0 = R0.keys(att, 2, M)
            return ((k0[0][0][0] - k1[0][0][0]) ** 2 + (k0[0][0][2] - k1[0][0][2]) ** 2) ** 0.5
        gap = gap_of(None)
        if a['game'] in FACING_GAP and a.get('turn') != 'off':
            # turned (and down -z by the game's gap) or as stored: the closer start wins - a pair starts within
            # reach. 2K15's standing grapples are stored turned (AA 2: 142 units turned, 1641 as stored); its
            # ground holds aren't (Patriot Lock: 1455 as stored, 2954 turned - the attacker already at the lying
            # victim's feet; turned without the shift, its YMBs attacker held one pose in-game)
            turned = gap_of(WR.facing_frame(FACING_GAP[a['game']]))
            if turned < gap:
                gap, face = turned, WR.facing_frame(FACING_GAP[a['game']])
                rep['notes'].append("attacker turned into the victim's frame (%s stores it turned, %d units off)" %
                                    (a['game'], FACING_GAP[a['game']]))
            else:
                rep['notes'].append('attacker as stored (the turn would start the pair %.0f units apart)' % turned)
        rep['notes'].append('source start gap %.0f units; root heights at the start: attacker %.0f, victim %.0f' %
                            (gap, R0.keys(att, 2, face)[0][0][1], k1[0][0][1]))
    tid = int(a['template']) if a.get('template') else None
    if tid and not usable11(g11.waze, tid):
        rep['warnings'].append("template %d is a move the game can't play (no move record): picked another" % tid)
        tid, a = None, dict(a, template=None)
    if not tid and not cat11:   # (a coded category, 2K14+: the 2011 move of the same name, else its category's)
        same = {norm(nm): i for i, (bits, nm) in g11.waze.items() if usable11(g11.waze, i)}.get(norm(name))
        if same and any(kd == 'YMKs' and p.startswith('MVMT/') for p, kd, xs in g11.banks_of(same)):
            tid = same
            rep['notes'].append('template: the 2011 move of the same name')
        else:
            cat11 = coded_category(g, g11, cat0)
            if cat11:
                rep['notes'].append('category %s = 2011 %s (by the moves both games name alike)' % (cat0.strip(), '/'.join(cat11)))
    vic_y = k1[0][0][1] if vic is not None else None
    # A dive from the top rope onto a lying victim (a corner move, the victim lying, the attacker's root up high -
    # 2K15's starts on the turnbuckle), or one laid out as 2011's (branched_dive): 2011's dives (Frog Splash 3555 ...)
    # are the attacker's alone - the victim takes the hit with the game's own reaction - so the template is a
    # one-person move and the source's victim track is left out. Before, 2K15's Superfly Splash got a two-person
    # corner move (Corner Head Scissors Flip) and was called off in 5 frames.
    corner = 'CORN' in (cat11 if isinstance(cat11, list) else [cat11.strip() if cat11 else ''])
    # (laid out as 2011's: the flight at x 0, the branches at theirs - onto a lying victim only: WWE '13's Diving Double
    # Axe Handle 1, onto a standing one (victim root -877), is a two-person corner move)
    # (a dive given as the template - the Mod Maker's template pick - is a dive all the same: before, the flight,
    # its branches, the turn and the travel were left out and the source's x 30 took x 0)
    opt_dive = bool(tid and a.get('template') and any(p.startswith('MVMT/CORN') for p, kd, ks in g11.banks_of(tid)) and
                    not any((0, 1) in ks for p, kd, ks in g11.banks_of(tid) if kd == 'YMKs'))
    if (not tid or opt_dive) and corner and flight is not None and (vic_y is None or vic_y > -400):
        att, vic, branched_dive = flight, None, True
        nf = max(1, (att['frames'] - 1) // 2)
        rep['notes'].append("source block 00000000 the attacker's flight (%d frames at 60 Hz), %s the branches" %
                            (att['frames'], ' / '.join(p for p in pairs if p.isdigit() and 30 <= int(p) < 90)))
    att_keys = R0.keys(att, nf, face)
    if (not tid or opt_dive) and (branched_dive or (vic is not None and corner and vic_y > -400 and
                                                    min(k[0][1] for k in att_keys) < -1100)):
        branches = sorted({int(p) for p, d in by.items() if p.isdigit() and 30 <= int(p) < 90 and 0 in d})
        kin = dive_kin(g11, name) or None
        dive = tid if opt_dive else next(
            (t for allowed in ([kin] if kin else []) + [None] for t in (
                choose_template(g11, ['CORN'], nf, None, allowed, paired=False, attacker_end_y=att_keys[-1][0][1],
                                need_x=branches, ringside=False),
                choose_template(g11, ['CORN'], nf, None, allowed, paired=False,   # (dives are corner moves; SPE1
                                attacker_end_y=att_keys[-1][0][1], ringside=False)) if t), None)   # has strikes)
        if kin and dive in kin and not opt_dive:
            rep['notes'].append('template: a 2011 dive of a like name (%s)' % g11.waze[dive][1])
        if dive:
            tid, vic, gap = dive, None, None
            rep['notes'].append("a dive onto a lying victim: the attacker's motion on a one-person template, the "
                                "victim reacts as in the game's own dives")
            # (the flight goes the template's way: 2011's corner climb ends facing the ring and the dive leaves from
            # there - WWE '13's Diving Corkscrew Splash, kept as stored, flipped backwards out of the ring. One person:
            # only the facing matters, the start is anchored to the template's below)
            tt = template_travel(g11, dive)
            if os.environ.get('MOVES_PORT_TRACE'):   # (debug: the flight's root path and the template's travel)
                log('trace flight %s, template travel %s' % ([tuple(round(v) for v in k[0]) for k in att_keys[::4]], tt))
            st = (att_keys[-1][0][0] - att_keys[0][0][0], att_keys[-1][0][2] - att_keys[0][0][2])
            # (to the template's heading, not only turned round: WWE '13's Diving Double Axe Handle 1 flew 37 degrees
            # off its template's and left the ring at an angle off the turnbuckle)
            turn, ang = heading_turn(st, tt)
            if turn is not None:
                face = turn if face is None else turn @ face
                att_keys = R0.keys(att, nf, face)
                rep['notes'].append("dive turned %.0f degrees to its template's heading (flight %.0f, %.0f vs %.0f, %.0f)"
                                    % (math.degrees(ang), st[0], st[1], tt[0], tt[1]))
    # A hold with phases (hold, struggle loops, escape, tap: two-person phases 1, 2 and 4 among those under 30 -
    # 2011's x 1 / 2 / 4; 2K17's Blue Thunder Bomb, a powerbomb with phases 1 / 2 / 3, got Walls of Jericho when 3
    # phases were enough) is a submission:
    # 2011's are YMBs moves in MVMT/ANAS (x 0 1 2 4 6 7). A coded category mixes them with other moves - 2K15's
    # M013 is mostly reversals, so its vote said STAT and the Patriot Lock got Mule Kick Reversal 2. (30 and up
    # are a dive's hit / miss branches - WWE '12's Shooting Star Press 4 has 30 / 40 / 50 - and 90-92 reversals.)
    src_phases = {int(p) for p, d in by.items() if p.isdigit() and 0 < int(p) < 30 and 0 in d and 1 in d}
    if not tid and vic is not None and {1, 2, 4} <= src_phases:
        # (where the source victim is - at the start and in the hold, phase 2: standing or on the mat - in model units,
        # as the YMBs templates' root heights)
        hh = None
        try:
            p2 = next(p for p, d in by.items() if p.isdigit() and int(p) == 2 and 1 in d)
            hk = R0.keys(by[p2][1], max(2, (by[p2][1]['frames'] - 1) // 2))
            hh = (R0.keys(vic, 2)[0][0][1] / 100.0, sorted(k[0][1] for k in hk)[len(hk) // 2] / 100.0)
        except (StopIteration, IndexError, KeyError, TypeError):
            hh = None
        sub = choose_submission(g11, nf, src_phases, hh)
        if sub:
            tid, cat11 = sub, ['ANAS']
            rep['notes'].append('a submission (source phases %s): a 2011 submission (MVMT/ANAS) as the template' %
                                ', '.join(str(x) for x in sorted(src_phases)) +
                                ('; the victim at %.1f at the start, %.1f in the hold - %s %.1f / %.1f' % (
                                    hh[0], hh[1], g11.waze[sub][1], *(g11.hold_heights(sub) or (0, 0))) if hh else ''))
    # (a reversal as the template only for a reversal: WWE '12's G.T.S. and World's Strongest Slam, filed under
    # its STAT, got Ankle Lock Reversal)
    rev = 'reversal' in (name or '').lower()
    # (a corner move from up high onto a standing victim: 2011's own of that name - its diving axe handles, clotheslines
    # and cross bodies sit in no MVMT/CORN group, so the category pick never sees them; WWE '13's Diving Double Axe
    # Handle 1 got Roll Up, then Apron Elbow Attack)
    if not tid and vic is not None and corner and min(k[0][1] for k in att_keys) < -1000:
        base = norm(re.sub(r'\s+\d+$', '', name or ''))
        same = sorted(i for i, (bits, nm) in g11.waze.items() if usable11(g11.waze, i) and
                      norm(re.sub(r'\s+\d+$', '', nm)) == base and
                      any(kd == 'YMKs' and p.startswith('MOT/') for p, kd, xs in g11.banks_of(i)))
        if same:
            tid = same[0]
            rep['notes'].append('template: the 2011 move of the same name (a dive onto a standing victim)')
    if not tid:
        tid, score = choose_template(g11, cat11, nf, gap, reversals=rev, with_score=True)
        # (a two-person move filed under STAT - strikes and reversals: WWE '12's G.T.S. got Pull-Back Armbar - or a
        # poor fit in its category's own moves also looks among the standing grapples; the better fit wins)
        stat = 'STAT' in (cat11 if isinstance(cat11, list) else [cat11.strip() if cat11 else ''])
        # (not for a corner move from up high - WWE '13's Diving Double Axe Handle 1 onto a standing victim got Roll Up)
        high = corner and min(k[0][1] for k in att_keys) < -1000
        if vic is not None and (tid is None or stat or score > 6) and cat11 not in (['ANAS'], 'ANAS') and not (high and tid):
            t2, s2 = choose_template(g11, ['STN1', 'STN2'], nf, gap, reversals=rev, with_score=True)
            if t2 is not None and (tid is None or s2 < score):
                rep['notes'].append('a poor fit in MVMT/%s (score %.1f): the standing grapples fit better (%.1f)' %
                                    ('/'.join(cat11) if isinstance(cat11, list) else cat11.strip(), score or 0, s2))
                tid, cat11 = t2, ['STN1', 'STN2']
    if not tid and not a.get('template'):   # (no two-person move of that category in 2011: any category)
        tid = choose_template(g11, '', nf, gap, reversals=rev)
        cat11 = 'any'
    if not tid or tid not in g11.waze:
        raise BadInput('no 2011 template move (category %s); give --template' % cat11)
    ymb = any(kd == 'YMBs' for p, kd, ks in g11.banks_of(tid))   # (a YMBs template - a taunt: a record in its thousand)
    lim = g11.id_limit(tid)
    if a.get('id'):
        new_id, why = int(a['id']), 'option'
        # (an id given by a caller - moveset_port's or the Mod Maker's pick - outside free_ids' rule is picked again:
        # past the template banks' last id it went into no bank (Mankind '13's Mandible Claw on 17038), 16000+ outside
        # the template's thousand the game never starts it (move_packs.cpp NeverStarts - 2.1.9's Brock Lesnar 2K14 / 2K15
        # ports, reports 20261104-091730 / -093125), 9000-9999 are streamed motions)
        if new_id not in g11.free_ids(lim, near=tid if ymb else None):
            nid, why2 = g11.free_record(name, tid if ymb else None, lim, avoid=a.get('_avoid'))
            if nid:
                rep['warnings'].append("id %d isn't free for a move on %d's banks (%s): %d instead" % (
                    new_id, tid, 'a DLC move' if new_id in dlc_move_ids() else
                    'past their last id' if lim is not None and new_id > lim else
                    'one the game never starts' if new_id >= 16000 else 'a stock or streamed record', nid))
                new_id, why = nid, why2
    else:
        new_id, why = g11.free_record(name, tid if ymb else None, lim)
    if not new_id:
        raise RuntimeError('no free 2011 move record')
    rep['notes'].append('template %d %s (%s)' % (tid, g11.waze[tid][1], 'option' if a.get('template') else
                                                  'MVMT/%s, the closest start gap and length' % ('/'.join(cat11) if isinstance(cat11, list) else cat11.strip())))
    rep['notes'].append('2011 id %d (%s)' % (new_id, why))
    pack = re.sub(r'[^a-z0-9]+', '_', name.lower()).strip('_')[:40] or 'move_%d' % mid
    pd = os.path.join(outdir, pack)
    os.makedirs(os.path.join(pd, 'motions'), exist_ok=True)
    progress(40, 'retargeting')
    R = WR.Retarget(bind='rest', twist=True, ik=a['game'] != 'wwe12')
    # (a two-person corner move from up high - a dive onto a standing victim - flies the template's heading too: both
    # people turn alike, the pair keeps its relation; the attacker's start is anchored to the template's below)
    vface = None
    if vic is not None and corner and min(k[0][1] for k in att_keys) < -1000:
        st = (att_keys[-1][0][0] - att_keys[0][0][0], att_keys[-1][0][2] - att_keys[0][0][2])
        turn, ang = heading_turn(st, template_travel(g11, tid))
        if turn is not None:
            face, vface = (turn if face is None else turn @ face), turn
            rep['notes'].append("both people turned %.0f degrees to the template's heading" % math.degrees(ang))
    keys_of = {0: [R.keys(att, nf, face), R.keys(vic, nf, vface) if vic is not None else None]}
    # The victim in 2011's pair layout (PAIR_GAP): the game turns his root by his own facing - the attacker's turned 180
    # degrees - from a base PAIR_GAP ahead, so his track is the source's victim seen from there: the attacker's frame
    # (face) moved by the attacker's start anchor (offset) and the gap, turned 180. Before, he took the attacker's frame
    # and anchor as they were: an attacker kept as stored (Mankind '13's Two Hand Bulldog: both facing one way in the
    # source) had his victim's travel mirrored, and a turned one (the Mandible Claw) his victim 2 x the anchor off
    offset = None
    victim_placed = False
    r0 = template_start(g11, tid) if vic is not None and vface is None else None
    if r0:
        import numpy as np
        a0 = keys_of[0][0][0][0]
        offset = (r0[0] - a0[0], 0.0, r0[2] - a0[2])
        k = ymks.ROOT_UNIT / 0.1   # (root units -> the source pose's, as facing_frame)
        Sh = np.eye(4)
        Sh[0, 3], Sh[2, 3] = offset[0] * k, (offset[2] + PAIR_GAP) * k
        vface = np.diag([-1.0, 1.0, -1.0, 1.0]) @ Sh @ (face if face is not None else np.eye(4))
        keys_of[0][1] = R.keys(vic, nf, vface)
        victim_placed = True
        rep['notes'].append("victim placed in 2011's pair layout (facing the attacker, %d root units ahead)" % PAIR_GAP)
    # (a one-person dive's travel is the game's: in a stock dive the attacker's own root moves a few units and the game
    # carries his base to the target during the dive - Diving Double Axe Handle 3538: 52 root units. A ported flight
    # kept WWE '13's own 1600 on top of the game's and landed metres past or beside the target. Its horizontal travel
    # is scaled to the template's; the heights - the jump - are kept)
    one_person = not any((0, 1) in ks for p, kd, ks in g11.banks_of(tid) if kd == 'YMKs')
    travel_k = 1.0
    if one_person and corner and keys_of[0][0]:
        a0 = keys_of[0][0]
        src_t = math.hypot(a0[-1][0][0] - a0[0][0][0], a0[-1][0][2] - a0[0][0][2])
        tt = template_travel(g11, tid)
        if tt and src_t > 1.5 * math.hypot(*tt) and src_t > 200:
            travel_k = math.hypot(*tt) / src_t
            keys_of[0][0] = scale_travel(a0, travel_k)
            rep['notes'].append('flight travel scaled to the template (%.0f -> %.0f root units: the game carries a dive '
                                'to its target)' % (src_t, math.hypot(*tt)))
    if os.environ.get('MOVES_PORT_TRACE'):   # (debug: both people's root paths, every 4th key)
        for who, ks in (('attacker', keys_of[0][0]), ('victim', keys_of[0][1] or [])):
            log('trace %s %s' % (who, [tuple(round(v) for v in k[0]) for k in ks[::4]]))
    xs = [0]
    # A multi-phase source move (a submission: hold, struggle loops, escape, tap) has a block pair per phase, and
    # phase n is 2011's x n: 2K15's Patriot Lock 0000 / 0001 / 0002 / 0004 / 0006 / 0007 against Ankle Lock 3's
    # x 0 / 1 / 2 / 4 / 6 / 7, of like lengths. Each template x takes its own phase (before, every x got phase 0's
    # motion: the hold never turned into the struggle).
    phase_blocks = {}
    if vic is not None or branched_dive:   # (a dive's 0030 / 0040 / 0050 pairs: 2011's x 30 / 40 / 50)
        for ph, d in by.items():
            if not ph.isdigit():   # (decimal: 2K15's 0090 / 0091 / 0092 are 2011's reversal x 90-92)
                continue
            px = int(ph)
            if 0 < px < 90 and 0 in d and 1 in d and d[0]['frames'] > 2:
                phase_blocks[px] = (d[0], d[1], max(1, (d[0]['frames'] - 1) // 2))
            # (a dive's miss, 0050, is the attacker alone - he lands on the mat: without it here the template's x 50
            # took the flight (x 0) again, and every ported dive that missed flew twice; 2011's x 50 goes on from the
            # air, as the source's)
            elif branched_dive and 30 <= px < 90 and 0 in d and d[0]['frames'] > 2:
                phase_blocks[px] = (d[0], None, max(1, (d[0]['frames'] - 1) // 2))
    if phase_blocks:
        rep['notes'].append('source phases %s = 2011 x of the same number' % ', '.join(str(x) for x in sorted(phase_blocks)))

    scaled_x = set()

    def phase_keys(x):
        if x not in keys_of and x in phase_blocks:
            ax, vx, nx = phase_blocks[x]
            ka, kv = R.keys(ax, nx, face), R.keys(vx, nx, vface) if vx is not None else None
            if os.environ.get('MOVES_PORT_TRACE'):   # (debug: a phase's attacker root heights)
                log('trace x%d %s' % (x, [round(k[0][1]) for k in ka]))
            keys_of[x] = [ka, kv]
        if branched_dive and x >= 30 and x not in keys_of:   # (a branch the source lacks: the template's own)
            return [None, None]
        if x in keys_of and travel_k != 1.0 and x not in scaled_x:   # (the branches too, from their own start)
            keys_of[x][0] = scale_travel(keys_of[x][0], travel_k)
            scaled_x.add(x)
        return keys_of.get(x, keys_of[0])
    lines = ['wazecopy %d %d' % (new_id, tid), 'wazename %d %s' % (new_id, name)]
    progress(60, 'tracks')
    nmot = 0
    ymb_ns, ymb_solved, ymb_built = None, {}, {}
    ymb_shift = None   # (YMBs: the source moved so its attacker starts where the template's does - as the YMKs path)
    landing = {}   # (a dive's drop to the mat: from the keyframe bank, the packed copies too - fall_tail)
    if any(kd == 'YMBs' for p, kd, ks in g11.banks_of(tid)):   # (the slow limb-IK path: more time - YMBS_EXTRA)
        extend_deadline()
    for path, kind, ks in g11.banks_of(tid):
        if kind == 'YMBs':   # (taunts, submissions: ADPCM channels with limb IK - ymbs_retarget)
            import ymbs
            import ymbs_retarget as YR
            pac, raw = g11.bank_raw(path)
            bank = ymbs.Bank(raw)
            ends = sorted({e[3] for e in bank.dir} | {len(raw) - bank.base})
            packed = path.startswith('MOTP')
            for x, y in ks:
                k = bank.find(tid, x, y)
                if k is None:
                    continue
                typ, nseg, itv, pad, f0, f1, f2, f3 = bank.hdr[k]
                pa, pv, pn = phase_blocks.get(x, (att, vic, nf))
                blk = (pa if y == 0 else pv) if x < 90 and y in (0, 1) else None
                data = None
                if blk is not None:
                    ymb_ns = ymb_ns or YR.nodes11()
                    try:
                        if ymb_shift is None:   # (the template attacker's first root vs the source's, x / z)
                            ymb_shift = (0.0, 0.0)
                            k0 = bank.find(tid, 0, 0)
                            if k0 is not None:
                                try:
                                    t0 = YR.frame_values(bank.decode(k0)[0], 0)['root_t']
                                    s0 = YR.w13_sampler(R, att, face)(0.0)['root'][:3, 3]
                                    ymb_shift = (float(t0[0] - s0[0]), float(t0[2] - s0[2]))
                                    rep['notes'].append("YMBs tracks moved %.0f / %.0f to the template attacker's start"
                                                        % ymb_shift)
                                except (KeyError, IndexError, TypeError, ValueError):
                                    pass
                        import numpy as np
                        Msh = np.eye(4)   # (applied to the source's world pose in its own units: 10 x the targets')
                        Msh[0, 3], Msh[2, 3] = 10.0 * ymb_shift[0], 10.0 * ymb_shift[1]
                        Mf = (Msh @ face) if (face is not None and y == 0) else Msh
                        sample = YR.w13_sampler(R, blk, Mf)
                        sk = (x if x in phase_blocks else 0, y)
                        if sk not in ymb_solved:   # (the same motion in every bank: solved once)
                            ymb_solved[sk] = YR.solve(ymb_ns, sample, pn)
                        off = bank.dir[k][3]
                        ck = (sk, typ, raw[bank.base + off: bank.base + next(e for e in ends if e > off)])
                        if ck not in ymb_built:   # (banks with the same template motion: encoded once)
                            progress(60 + min(30, 8 * len(ymb_built)), 'tracks: %s x%d y%d' % (path, x, y))
                            d_, f_ = YR.build_motion(bank, ymb_ns, sample, pn, typ, ymb_solved[sk])
                            ymb_built[ck] = (d_, f_, YR.check(bank, d_, f_, typ, sample, ymb_ns))
                        data, frames, worst = ymb_built[ck]
                        rep['notes'].append('%s x%d y%d: YMBs motion, %d keys, worst joint %.2f units' %
                                            (path, x, y, frames, worst))
                        hdr = struct.pack('<BBBB4f', typ, 0, 1, 0, f0, f1, f2, f3)
                    except (StopIteration, IndexError) as ex:   # (a channel table this solver doesn't fill)
                        rep['warnings'].append('%s x%d y%d: the template\'s motion (%s)' % (path, x, y, ex or 'channels'))
                        data = None
                if data is None:   # (props, cameras, branches, the other person: the template's)
                    off = bank.dir[k][3]
                    data = raw[bank.base + off: bank.base + next(e for e in ends if e > off)]
                    frames = bank.dir[k][4]
                    hdr = raw[bank.base + 4 + 20 * k: bank.base + 24 + 20 * k]
                fn = 'motions/%d_%d_%d_%s.ymb' % (new_id, x, y, path.replace('/', '').lower())   # (per bank: channel tables differ)
                open(os.path.join(pd, fn), 'wb').write(data)
                lines.append('motion %s %s %d %d %d %d %s %s' % (pac, path, new_id, x, y, frames, fn, hdr.hex()))
                nmot += 1
            continue
        if kind != 'YMKs':
            rep['dropped'].append('bank %s (%s)' % (path, kind))
            continue
        pac, bank = g11.bank(path)
        packed = path.startswith('MOTP')
        for x, y in ks:
            e = bank.find(tid, x, y)
            if e is None:
                continue
            src_keys = phase_keys(x)[y] if y in (0, 1) and x < 90 else None
            if src_keys:
                tk = ymks.body_keys(e['data'], bank.T, packed)
                if offset is None and y == 0:   # (anchor: the template attacker's start, x / z)
                    r0 = next((k['root'] for k in tk if k.get('root')), None)
                    if r0:
                        offset = (r0[0] - src_keys[0][0][0], 0.0, r0[2] - src_keys[0][0][2])
                # (a placed victim has the anchor in his frame already)
                off = (0.0, 0.0, 0.0) if (y == 1 and victim_placed) else (offset or (0.0, 0.0, 0.0))
                tev = [(k['frame'], k['events']) for k in tk if k.get('events')]
                lim = 0x20 if packed else 0x7f
                drop, tail = fall_tail(tk) if (x, y) == (0, 0) and not packed else (None, [])
                if packed and (x, y) == (0, 0):   # (the packed copy keys every 5 frames: the keyframe bank's drop)
                    drop, tail = landing.get('drop', (None, []))
                if tail and src_keys[-1][0][1] < src_keys[0][0][1] + 300:   # (ends as high as it left from)
                    # A source flight that ends in the air (WWE '12's Diving Double Axe Handle: its own engine brought
                    # the base down) hung at turnbuckle height in 2011, the root never reaching the mat: the
                    # template's drop goes on after it, the last pose held (copying the template's 7000 onto the last
                    # key alone froze it in the air).
                    landing['drop'] = (drop, tail)
                    (r1, a1), ns = src_keys[-1], len(src_keys) - 1
                    src_keys = src_keys + [((r1[0], r1[1] + d, r1[2]), a1) for d in tail]
                    n = len(src_keys) - 1
                    evs = retime([(f, ev) for f, ev in tev if f < drop], ns / float(max(1, drop)), ns, tid, new_id, lim)
                    for f, ev in retime([(f - drop, ev) for f, ev in tev if f >= drop], 1.0, n - ns - 1, tid, new_id,
                                        lim).items():
                        evs.setdefault(ns + 1 + f, []).extend(ev)
                    if not landing.get('noted'):
                        landing['noted'] = True
                        rep['notes'].append("the template's drop to the mat after the flight (%d frames: the game lands "
                                            "a dive when the attacker's root reaches the mat)" % len(tail))
                else:
                    n = len(src_keys) - 1
                    evs = retime(tev, n / float(max(1, e['frames'])), n, tid, new_id, lim)
                # A key at frame f with count c is the pose the body reaches at f + c + 1 (ymks.build_motion). The
                # game's own copies: keyframe banks a key every frame (count 0: the next frame's pose), the packed
                # match copies (MOTP) a key every 5 frames (count 4: the pose 5 frames on) - a packed copy keyed
                # every frame (4x the stock size) froze both people in-game after ~60 frames while the motion's
                # frame ran on past its end (F-5 1, 2K15). A directory's frames = the stream's frame count.
                step = 5 if packed else 1

                def pose(f):
                    root, ang = src_keys[min(f, n)]
                    bones = {k: {ax: ang[k]['xyz'.index(ax)] for ax in ymks.axes_of(ymks.TYPES_M1[k])} for k in range(21)}
                    return (root[0] + off[0], root[1], root[2] + off[2]), bones

                ks_ = []
                for f in range(0, n + 1, step):
                    root, bones = pose(f + step)
                    ks_.append(dict(frame=f, count=step - 1, root=root, bones=bones, events=evs.pop(f, [])))
                for f, ev in sorted(evs.items()):
                    ks_.append(dict(frame=f, count=0, root=None, bones=None, events=ev))
                data, frames = ymks.build_motion(ks_, n + 1, packed=packed), n + 1
            else:
                data, frames = e['data'], e['frames']   # (props, cameras, reversal branches: the template's)
                # (a camera, y 50-57, of a phase the source has: timed to the ported length, as the events are - a
                # template's 99-frame camera on a 144-frame move ended early and held its last shot; Goldberg 2K14's
                # finishers, report 20261104-103857)
                body = phase_keys(x)[0] if 50 <= y <= 57 and x < 90 else None
                if body and frames and len(body) != frames:
                    try:
                        data, frames = retime_camera(e['data'], len(body) / float(frames), len(body), tid, new_id), len(body)
                    except (ValueError, IndexError):
                        data, frames = e['data'], e['frames']
            fn = 'motions/%d_%d_%d%s.ymk' % (new_id, x, y, '_p' if packed else '')
            open(os.path.join(pd, fn), 'wb').write(data)
            lines.append('motion %s %s %d %d %d %d %s' % (pac, path, new_id, x, y, frames, fn))
            nmot += 1
    for kind, grp, r, ev in g11.records(tid):
        r = bytearray(r)
        struct.pack_into('<H', r, 0, new_id)
        lines.append('%s %d %s%s' % (kind, grp, r.hex(), (' ' + ev.hex()) if kind == 'evt' else ''))
    open(os.path.join(pd, 'pack.txt'), 'w', newline='\n').write('\n'.join(lines) + '\n')
    rep['notes'].append('%d motions in %d banks, %d frames at 30 Hz' % (nmot, len(g11.banks_of(tid)), nf))
    rep['dropped'].append('finger poses (the template\'s); the source\'s own sounds and damage timing (the template\'s, retimed)')
    for sk in getattr(g, 'skipped', []):
        rep['warnings'].append('not readable, its moves left out: ' + sk)
    if getattr(g, 'placeholders', None):
        rep['notes'].append('%s: DLC stand-ins on the disc (no moves; the DLC packages bring them)'
                            % ', '.join(os.path.basename(p) for p in g.placeholders))
    # (the choices made, for the Mod Maker's move inspector)
    rep['options'] = dict(template_how='option' if a.get('template') else next(
        (n for n in rep['notes'] if n.startswith('template: ')), 'the closest start gap and length'),
        category_map=cat11 if isinstance(cat11, list) else (cat11.strip() or None), attacker_turned=face is not None,
        id_limit=g11.id_limit(tid))
    rep.update(ok=True, out=pd, folder=pd, id=new_id, name=name, template=tid)
    progress(100, 'done')
    return rep


def open_game(a, src, work=None):
    if a['game'] in OLD:
        return Game0789(src, work or os.path.join(HERE, 'work', 'm0789_' + a['game']))
    if a['game'] in GAME_CLASS and a['game'] != 'wwe12':
        return GAME_CLASS[a['game']](src, a.get('svrmod'))
    return Game12(src)


def cmd_list(a):
    sp = __import__('superstar_port')
    src = sp.Src(a['src'], os.path.join(a.get('cache') or os.path.join(HERE, 'work', 'src'), a['game']))
    g = open_game(a, src)
    idx = g.index()
    import svr10_superstar as SS
    w11 = SS.Game(a['pac11'], 11).waze()
    by11 = {}
    for i, (bits, nm) in w11.items():
        by11.setdefault(norm(nm), i)
    out = []
    for mid in sorted(idx):
        nm = g.waze.get(mid, '')
        cat = idx[mid][min(idx[mid])][0]
        i11 = by11.get(norm(nm)) if nm else None
        out.append(dict(key=str(mid), name=nm, category=cat.strip(), variants=len(idx[mid]), in_2011=i11))
    return out


def main(argv):
    if len(argv) < 2 or argv[1] in ('-h', '--help'):
        print(json.dumps({'error': 'usage: games | list --game G --src S | convert --game G --src S --item K --out D'}))
        return 2
    cmd = argv[1]
    a = dict(pac11=PAC11)
    i = 2
    while i < len(argv):
        k = argv[i]
        if k.startswith('--') and i + 1 < len(argv):
            a[k[2:].replace('-', '_')] = argv[i + 1]
            i += 2
        else:
            print(json.dumps({'error': 'unexpected argument: %s' % k}))
            return 2
    if a.get('dlc'):   # (the source game's DLC packages: Src reads them beside the disc)
        os.environ['SVR2011_PORT_DLC'] = a['dlc']
    rep = dict(ok=False, out=None, folder=None, opens='moves', warnings=[], dropped=[], notes=[])
    rep_path = None
    try:
        if cmd == 'games':
            res = GAMES
        elif cmd == 'renumber':
            if not a.get('mod'):
                raise BadInput('renumber needs --mod <installed mod folder>')
            res = renumber(a)
        elif cmd in ('list', 'convert'):
            if a.get('game') not in {g['id'] for g in GAMES}:
                raise BadInput('unknown --game %r (see games)' % a.get('game'))
            if a['game'] not in READY:
                raise BadInput('%s is not ready yet (status research)' % a['game'])
            if not a.get('src'):
                raise BadInput('--src is needed')
            if cmd == 'list':
                res = cmd_list(a)
            else:
                if not a.get('item') or not a.get('out'):
                    raise BadInput('convert needs --item and --out')
                rep_path = os.path.join(os.path.abspath(a['out']), 'report.json')
                res = convert(a, rep)
        else:
            raise BadInput('unknown command %r' % cmd)
    except (BadInput, FileNotFoundError) as e:
        print(json.dumps({'ok': False, 'error': __import__('superstar_port').missing(__import__('superstar_port').rel_to(e.filename or str(e), a.get('src')), a.get('src'))
                          if isinstance(e, FileNotFoundError) else str(e)}))
        return 2
    except Exception as e:   # noqa: BLE001
        if type(e).__name__ == 'BadInput':   # (superstar_port's: Src.need, a missing file)
            print(json.dumps({'ok': False, 'error': str(e)}))
            return 2
        traceback.print_exc(file=sys.stderr)
        res = dict(rep, ok=False, error='%s: %s' % (type(e).__name__, e))
        if rep_path:
            json.dump(res, open(rep_path, 'w'), indent=1)
        print(json.dumps(res))
        return 3
    if rep_path:
        json.dump(res, open(rep_path, 'w'), indent=1)
    print(json.dumps(res, indent=None if cmd == 'convert' else 1))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
