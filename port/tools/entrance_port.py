"""Entrances from other games -> SvR 2011 entrances: one entry point for the Mod Maker.

  python entrance_port.py games
  python entrance_port.py list --game <id> --src <extract folder or disc image> [--game-pac <2011 pac dir>]
  python entrance_port.py convert --game <id> --src <...> --entrance <n> --number <2011 n> --out <dir>
                                  [--name <text>] [--no-parts] [--game-pac <2011 pac dir>]
                                  [--titantron [<movie n>]] [--bink2-tool <path>] [--no-pyro]
  (wwe13 also takes --svrmod <svrmod.exe>: its pacs are xcompressed; default: next to the tools or
   the Mod Maker; missing = exit 2)
  python entrance_port.py free-numbers [--game-pac <2011 pac dir>] [--mods <Mods dir>]
  --kind victory (list / convert / free-numbers): the victory scenes (pac/evt/Winning*.pac) instead of the
        entrances - SvR 2008, 2009, 2010 and Legends of WrestleMania; --number 6000..7999, written as
        `pacentry evt\Winning0.pac EVPA|EVTA <n>` (a superstar mod picks it with victory=<n>,
        docs/VICTORY_SCENES.md); no Create-An-Entrance parts, pyro or titantron
  python entrance_port.py detect --src <one entrance file>
        -> {ok, game, name, platform, entrances, file, detected, limits[]} (list / convert take the same file)

Contract (shared by the Mod Maker's converters, docs/MOD_MAKER_3_PLAN.md):
- stdout carries only the JSON result; logs go to stderr, progress lines on stderr read exactly
  `progress <0-100> <short text>`.
- exit 0 = done, 2 = bad input (unknown game, missing file, bad number), 3 = conversion failed;
  anything else is a crash.
- convert writes <out>/pack/ (a move pack: pack.txt with `pacentry` lines + entrance/*.bin; it
  goes in a superstar mod's moves/ or in Mods/Moves/<name>/, docs/MOVE_PACKS.md) and
  <out>/report.json {ok, game, entrance, number, name, files, warnings[], dropped[], notes[]},
  mirrored to stdout.
- pyro: for games whose particle packs are 2011's format (svr10, wwe12, wwe13, wwe2k14) the
  entrance's own set, pac/prtcls.pac PCE6 / LPE6 <0NNN> (keyed by entrance number), goes into the
  pack as `pacentry prtcls.pac PCE6|LPE6 <0nnn> pyro/...` (an EPAC entry; the game's move-pack
  overlay writes it into a copy of prtcls.pac). 2K14 has no LPE6: PCE6 is used for both (2011's
  own are the same for 249 of 290). --no-pyro leaves it out.
- --titantron: also copy the source game's titantron movies/titantron/<n>.* (n = the movie number
  given, else the source entrance number) to <out>/movie.bik (Bink 1, as is) or <out>/movie.mp4
  (Bink 2, WWE '13 / 2K14, through tools/bink2_convert.py and a decoder the player installed;
  --bink2-tool picks it). The mod then takes movie=movie.bik / movie.mp4 (the Mod Maker or the
  launcher makes the .bik fallback beside an .mp4). A missing file or decoder is a note, never a
  failure.
- --src is an extract folder or a disc image (tools/discfs.py when present: Xbox 360 XDVDFS /
  PS2 ISO 9660); paths inside it are the disc's own (e.g. pac/evt/Nyujyo0.pac, PAC/EVTENT.PAC).
- --src may also be ONE entrance file copied off a disc (any name): a 360 pac/evt pac (SvR 2007
  evt1/2.pac, SvR 2008-WWE '12 Nyujyo*.pac, WWE '13-2K15 Entrance*.pac, 2K16 / 2K17 <entrance>.pac -
  keep that name or give --entrance) or a PS2 PAC/EVTENT.PAC (HCTP, SvR 2004, SYM), PAC/EVT.PAC
  (SvR 2006), EN3.PAC (JBI).  --game is then optional (detected from the format; --game wins).  The
  motion converts in full - script, every track, camera - as every game keeps them in that file.  Not
  in a lone file (report.lone.limits, also in notes): the theme, the titantron, pyro, PS2 names
  (numbers only), JBI's hand poses (fingers stay in the ring pose).  WWE '13+ pacs still need svrmod.

Games: docs/OTHER_GAMES_ENTRANCES.md (formats and what converts); per-game converters
tools/svr08_entrance.py (SvR 2008), tools/svr10_entrance.py (SvR 2010), tools/hctp_entrance.py (HCTP)
and tools/wwe13_entrance.py (WWE '13).
"""
import collections
import io
import json
import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import svrfmt  # noqa: E402
import ymks  # noqa: E402
import svr08_entrance as E08  # noqa: E402
import hctp_entrance as EH  # noqa: E402
import entrance_props as EPR  # noqa: E402
import svr10_entrance as E10  # noqa: E402
import wwe13_entrance as E13  # noqa: E402


class InputError(Exception):
    """bad input: exit 2"""


def log(*a):
    print(*a, file=sys.stderr)


def progress(pct, text):
    print('progress %d %s' % (pct, text), file=sys.stderr, flush=True)


# ============================================================================ sources

class FolderSource:
    """an extract folder; the same calls as discfs's disc objects"""

    def __init__(self, root):
        self.root = root

    def _p(self, path):
        p = os.path.join(self.root, *path.replace('\\', '/').split('/'))
        if os.path.exists(p):
            return p
        # case-insensitive walk (disc paths are matched without case)
        cur = self.root
        for part in path.replace('\\', '/').split('/'):
            if not part:
                continue
            hit = next((n for n in os.listdir(cur) if n.lower() == part.lower()), None) if os.path.isdir(cur) else None
            if hit is None:
                return None
            cur = os.path.join(cur, hit)
        return cur

    def exists(self, path):
        return self._p(path) is not None

    def listdir(self, path=''):
        p = self._p(path) if path else self.root
        return sorted(os.listdir(p)) if p and os.path.isdir(p) else []

    def read(self, path):
        p = self._p(path)
        if p is None:
            raise InputError('not in the source: %s' % path)
        with open(p, 'rb') as f:
            return f.read()

    def size(self, path):
        return os.path.getsize(self._p(path))


def open_source(path):
    if not path or not os.path.exists(path):
        raise InputError('--src not found: %s' % path)
    try:
        import discfs  # (the Mod Maker's disc reader, D1: folders, X360 XDVDFS, PS2 ISO 9660)
        import dlc_src  # (superstar_port --dlc: the game's packages beside the disc)
        return dlc_src.view(discfs.open(path))
    except ImportError:
        if os.path.isdir(path):
            return FolderSource(path)
        raise InputError('reading a disc image needs tools/discfs.py; give an extract folder instead')
    except Exception as x:
        raise InputError('cannot read --src %s: %s' % (path, x))


# ============================================================================ a lone entrance file
# --src may be one entrance pac copied off a disc (no disc, no folder).  Every supported game keeps an
# entrance's script and all its tracks - camera included - in that one file (checked on every stock pac:
# 2007-2K15 EVP + EVT/EVZ side by side in the same pac, 2K16 / 2K17 one pac per entrance, PS2 the whole
# set in one file), so the motion converts in full.  What a lone file can't give: lone_limits().

LONE_MAGIC = (b'EPAC', b'EPK8', b'DPAC', b'HSPC', b'\x0f\xf5\x12\xed')
XCOMPRESS = b'\x0f\xf5\x12\xed'
LONE_PATH = {   # game -> the path the game's reader looks for (the lone file is served there)
    'svr07': 'pac/evt/evt1.pac', 'svr08': 'pac/evt/Nyujyo0.pac', 'svr09': 'pac/evt/Nyujyo0.pac',
    'low': 'pac/evt/Nyujyo0.pac', 'svr10': 'pac/evt/Nyujyo0.pac', 'wwe12': 'pac/evt/Nyujyo0.pac',
    'wwe13': 'pac/evt/Entrance00.pac', 'wwe2k14': 'pac/evt/Entrance00.pac', 'wwe2k15': 'pac/evt/Entrance00.pac',
    'wwe2k16': 'pac/evt/%05d.pac', 'wwe2k17': 'pac/evt/%05d.pac',
    'hctp': 'PAC/EVTENT.PAC', 'svr04': 'PAC/EVTENT.PAC', 'sym': 'PAC/EVTENT.PAC', 'svr06': 'PAC/EVT.PAC',
    'jbi': 'EN3.PAC'}


def lone_limits(g):
    """what the disc has beside the entrance file (said in every lone-file report)"""
    out = ['the theme music is not in any entrance file (pick the song for the mod)',
           'the titantron movie is on the disc (movies/titantron), not in the entrance file: --titantron needs the disc']
    if g in PYRO_GAMES:
        out.append('pyro (the particle set) is in another pac (pac/prtcls.pac; 2K16 / 2K17 pac/particle/pce6NNNN.pac): '
                   'not converted from a lone file')
    if g in ('hctp', 'svr04', 'sym'):
        out.append('entrance names are in PAC/MOVES.PAC on the disc: the list shows numbers only')
    if g == 'jbi':
        out.append("names are in the game's ELF and the hand poses in M.PAC on the disc: numbers only, and the "
                   "fingers keep their ring pose (JBI's pose ids mean nothing without its pose library)")
    return out


def _norm(p):
    return p.replace('\\', '/').strip('/').lower()


class LoneSource:
    """one entrance file served at the path its game reads (discfs's calls: exists / isdir / listdir / read /
    size / info / walk); everything else on the 'disc' is missing"""

    def __init__(self, path, vpath, data):
        self.path, self.vpath, self._data = path, vpath, data

    def exists(self, path):
        return _norm(path) == _norm(self.vpath) or self.isdir(path)

    def isdir(self, path):
        return _norm(path) == _norm(os.path.dirname(self.vpath)) != ''

    def listdir(self, path=''):
        return [os.path.basename(self.vpath)] if _norm(path) == _norm(os.path.dirname(self.vpath)) else []

    def read(self, path, off=None, size=None):
        if _norm(path) != _norm(self.vpath):
            raise InputError('a lone entrance file has no %s' % path)
        d = self._data
        return d if off is None else d[off:off + (len(d) - off if size is None else size)]

    def size(self, path):
        return len(self.read(path))

    def info(self):
        return {}

    def walk(self):
        return [(self.vpath, len(self._data))]


def is_lone_file(path):
    if not path or not os.path.isfile(path):
        return False
    with open(path, 'rb') as f:
        return f.read(4) in LONE_MAGIC


def _evp_class(ents):
    """the most common EVP size % 4480 (an object = 21 x 212-byte slots + 28) = the header's size: SvR 2007 / 2008
    160, SvR 2009 / LoW 176, SvR 2010 240, WWE '12 224-288, WWE '13 / 2K14 128-192 (2K15's 788-byte slots: any)"""
    c = collections.Counter()
    for (g, n), b in ents.items():
        if g.startswith('EVP') and n.isdigit() and int(n) < 10000 and b[:4] == b'0FOP':
            c[struct.unpack_from('>I', b, 4)[0] % 4480] += 1
    return c.most_common(1)[0][0] if c else None


def _obj_bytes(ents):
    """the largest EVP bytes per cast object (+0x40 low 16 bits = objects): <= ~4.5 K with 212-byte slots, ~16.6 K
    with 2K15's 788-byte ones (2K15 pacs hold small EVPs too: the largest decides)"""
    v = [struct.unpack_from('>I', b, 4)[0] // max(1, struct.unpack_from('>I', b, 0x40)[0] & 0xffff)
         for (g, n), b in ents.items() if g.startswith('EVP') and b[:4] == b'0FOP' and len(b) >= 0x44]
    return max(v) if v else 0


def _first_evt(ents):
    return next((b for (g, n), b in sorted(ents.items()) if g.startswith('EVT') and n.isdigit() and int(n) < 9999), b'')


def detect_lone(path, data):
    """-> the game id of a lone entrance file, from its format (InputError: not an entrance file we read).
    PS2: the DPAC's first group - ENMT JBI, EVNT SvR 2006, EVET named rows SYM, EVET numbered + ENTR with
    an EVTM row SvR 2004, else HCTP.  X360: xcompressed pacs decoded (svrmod), HSPC 2K17, a scene pac 2K16;
    EVZ tracks: ~16.6 K EVP bytes per object or an xcompressed EPK8 with EVP2 2K15, xcompressed WWE '13 (EPK8) / 2K14 (EPAC), else WWE '12 (unless
    its EVP class says a decoded '13 / 2K14); EVT tracks: YMKs or class
    240 SvR 2010, 160 SvR 2007 (event size table op 127 = 0) / 2008, 176 LoW (op 72 sized) / SvR 2009."""
    name = os.path.basename(path)
    if data[:4] == b'DPAC':
        tag = data[0x800:0x804]
        if tag == b'ENMT':
            return 'jbi'
        if tag == b'EVNT':
            return 'svr06'
        if tag == b'EVET':
            n = struct.unpack_from('<H', data, 0x804)[0]
            if not n & 0x8000:
                return 'sym'
            off = 0x808 + 4 * (n & 0x7FFF)
            if data[off:off + 4] == b'ENTR':
                rows = [data[off + 8 + 8 * i:off + 12 + 8 * i] for i in range(struct.unpack_from('<H', data, off + 4)[0])]
                return 'svr04' if b'EVTM' in rows else 'hctp'
        raise InputError('%s: a PS2 DPAC, but not an entrance pac (EVTENT.PAC / EVT.PAC / EN3.PAC)' % name)
    d, dec = data, False
    if data[:4] == XCOMPRESS:
        try:
            d, dec = E13.xdec(data, E13.find_svrmod(SVRMOD)), True
        except E13.ToolMissing as x:
            raise InputError(str(x))
    if d[:4] == b'HSPC':
        return 'wwe2k17'
    if d[:4] not in (b'EPAC', b'EPK8'):
        raise InputError('%s: not an entrance pac (%r)' % (name, d[:4]))
    if dec and d[:4] == b'EPAC':
        try:
            if E13.scene_entries(d):
                return 'wwe2k16'                   # (one entrance per pac: 2K16, or a 2K17 EPAC one - same format)
        except Exception:
            pass
    try:
        ents = E13.epk8_entries(d) if dec else pac_entries(d)
    except Exception:
        ents = {}
    if not any(g.startswith('EVP') for g, n in ents):
        raise InputError('%s: an EPAC / EPK8 without entrance scripts (EVP)' % name)
    groups = {g[:3] for g, n in ents}
    cls = _evp_class(ents)
    if 'EVZ' in groups:
        if _obj_bytes(ents) > 10000 or dec and d[:4] == b'EPK8' and any(g == 'EVP2' for g, n in ents):
            return 'wwe2k15'                       # (788-byte slots; its group is EVP2, WWE '13's 'EVP ')
        if dec or cls is not None and cls < 200:   # (xcompressed, or one decoded by hand)
            return 'wwe2k14' if d[:4] == b'EPAC' else 'wwe13'
        return 'wwe12'
    t = _first_evt(ents)
    if cls == 240 or t[:4] == b'YMKs':
        return 'svr10'
    if cls == 160:
        return 'svr07' if len(t) > 127 and t[127] == 0 else 'svr08'
    if cls == 176:
        return 'low' if len(t) > 72 and t[72] else 'svr09'
    raise InputError('%s: an entrance pac, but its game was not recognised (EVP class %s): give --game' % (name, cls))


def lone_source(path, g=None, entrance=None):
    """-> (source, game, detected game) for a lone entrance file; g (--game) wins over the detection"""
    with open(path, 'rb') as f:
        data = f.read()
    found = detect_lone(path, data)
    g = g or found
    vp = LONE_PATH.get(g)
    if vp is None:
        raise InputError('%s entrances can not come from a lone file' % g)
    if '%05d' in vp:                               # (2K16 / 2K17: the entrance number is the file's name)
        m = re.match(r'(\d{5})\.pac$', os.path.basename(path), re.I)
        n = int(m.group(1)) if m else entrance
        if n is None:
            raise InputError('%s: name the file <entrance>.pac (00584.pac) or give --entrance' % os.path.basename(path))
        vp = vp % n
    return LoneSource(path, vp, data), g, found


# ============================================================================ containers

unbpe = svrfmt.unbpe


def epk8_read(d):
    """EPK8 (2011's evt/Nyujyo*.pac): TOC at 0x800 - groups {4CC, u16 count*4, u16, u32} each
    followed by 16-byte entries {name[8] space padded, u32 sector, u32 size/0x100} (LE); data from
    0x4000 in 0x800 sectors. -> {(group, name): raw bytes}"""
    out = collections.OrderedDict()
    p = 0x800
    while p + 12 <= 0x4000 and d[p:p + 4].strip(b'\0'):
        tag = d[p:p + 4].decode('latin-1')
        n = struct.unpack_from('<H', d, p + 4)[0] // 4
        q = p + 12
        for i in range(n):
            nm = d[q:q + 8].decode('latin-1').strip()
            sec, sz = struct.unpack_from('<II', d, q + 8)
            o = 0x4000 + sec * 0x800
            out[(tag, nm)] = d[o:o + sz * 0x100]
            q += 16
        p = q
    return out


def pac_entries(d, want=None):
    """EPAC or EPK8 -> {(group, name): unpacked bytes}; want(group, name): 0 leave it out, 1 unpacked, 2 as stored
    (BPE decoding is most of the time: a convert needs one scene, a list only the scripts)"""
    def take(k, v):
        w = want(*k) if want else 1
        return None if not w else unbpe(v) if w == 1 else v
    if d[:4] == b'EPK8':
        out = {k: take(k, v) for k, v in epk8_read(d).items()}
        return {k: v for k, v in out.items() if v is not None}
    if d[:4] == b'EPAC':
        # (svrfmt.epac_read's layout, but the count is the u16 at +4: SvR 2007 keeps other data in
        # the u16 at +6)
        out = collections.OrderedDict()
        p = 0x800
        while p < 0x4000 and d[p:p + 4] != bytes(4):
            ty = d[p:p + 4].decode('latin-1')
            cnt = struct.unpack_from('<H', d, p + 4)[0] // 3
            p += 12
            for _ in range(cnt):
                n = d[p:p + 4].replace(bytes(1), b'').decode('latin-1').strip()
                sec, sz = struct.unpack_from('<II', d, p + 4)
                o = 0x4000 + sec * 0x800
                v = take((ty, n), d[o:o + sz * 0x100])
                if v is not None:
                    out[(ty, n)] = v
                p += 12
        return out
    if d[:4] in (b'DPAC', b'DPK8'):          # (PS2 SvR 2007-2011: the 360 pacs in little-endian)
        import ps2_entrance
        return ps2_entrance.entries(d)
    raise ValueError('not an EPAC / EPK8 / PS2 DPAC / DPK8 pac')


def evp_name(evp):
    """the 30-byte name of an EVP (big-endian 2008/2011 '0FOP' chunk)"""
    if evp[:4] != b'0FOP':
        return ''
    return evp[0x22:0x40].split(b'\0')[0].decode('latin-1', 'replace').strip()


def norm(s):
    return re.sub(r'[^a-z0-9]', '', s.lower())


# ---- victory scenes (--kind victory): the same scripts and track banks as the entrances, in Winning*.pac
VICTORY = False
VICTORY_GAMES = ('svr08', 'svr09', 'svr10', 'low')
VICTORY_PAC, VICTORY_GROUPS = 'evt\\Winning0.pac', ('EVPA', 'EVTA')


def scene_rx():
    """the source's scene pacs: Nyujyo0-3 (entrances) or Winning0-3 (--kind victory)"""
    return r'(?i)winning\d\.pac$' if VICTORY else r'(?i)nyujyo\d\.pac$'


def stock_victories(game_pac):
    """{number: name} of 2011's own victory scenes (pac/evt/Winning*.pac)"""
    out = {}
    if game_pac:
        for f in sorted(os.listdir(os.path.join(game_pac, 'evt'))):
            if re.match(r'(?i)winning.\.pac$', f):
                for (g, n), raw in epk8_read(open(os.path.join(game_pac, 'evt', f), 'rb').read()).items():
                    if g.startswith('EVP') and n.isdigit():
                        out[int(n)] = evp_name(unbpe(raw))
    return out


def pack_victories(mods):
    """victory numbers the installed packs already add"""
    used = set()
    if mods and os.path.isdir(mods):
        for root, dirs, files in os.walk(mods):
            if 'PacOverlay' in root or os.path.exists(os.path.join(root, 'disabled')):
                continue
            if 'pack.txt' in files:
                for line in open(os.path.join(root, 'pack.txt'), encoding='latin-1'):
                    m = re.match(r'\s*pacentry\s+\S*winning\S*\s+EV[PT]\S\s+(\d+)', line, re.I)
                    if m:
                        used.add(int(m.group(1)))
    return used


def write_victory_pack(out, number, evp, evt, label):
    d = os.path.join(out, 'pack')
    os.makedirs(os.path.join(d, 'victory'), exist_ok=True)
    lines = ['# %s' % label]
    for g, b, suf in ((VICTORY_GROUPS[0], evp, 'evp'), (VICTORY_GROUPS[1], evt, 'evt')):
        fn = 'victory/%d_%s.bin' % (number, suf)
        open(os.path.join(d, fn), 'wb').write(b)
        lines.append('pacentry %s %s %d %s' % (VICTORY_PAC, g, number, fn))
    open(os.path.join(d, 'pack.txt'), 'w', newline='\r\n').write('\n'.join(lines) + '\n')
    return d


# ============================================================================ 2011 side

NYUJYO = [(100, 134, '0', '1'), (135, 178, '1', '2'), (179, 271, '2', '3'), (272, 290, '3', '4'),
          (291, 479, '4', '5'), (480, 511, '5', 'E'), (512, 531, '6', 'F'), (532, 551, '7', 'G'),
          (552, 599, '8', 'H'), (600, 999, '9', 'I')]


def find_game_pac(arg):
    """the 2011 pac folder: --game-pac, else the first ancestor of the working directory (or of
    this script) holding pac/evt/Nyujyo5.pac (the Mod Maker runs in <game>/Mods/.convert)"""
    if arg:
        if not os.path.exists(os.path.join(arg, 'evt', 'Nyujyo5.pac')):
            raise InputError('--game-pac has no evt/Nyujyo5.pac: %s' % arg)
        return arg
    for start in (os.getcwd(), HERE):
        p = os.path.abspath(start)
        for _ in range(8):
            for cand in (os.path.join(p, 'pac'), os.path.join(p, 'Game Files', 'pac')):
                if os.path.exists(os.path.join(cand, 'evt', 'Nyujyo5.pac')):
                    return cand
            p = os.path.dirname(p)
    return None


_stock = {}
_stock_evp = {}


def stock_2011_evps(game_pac):
    """{number: EVP bytes} of 2011's own entrances"""
    stock_2011(game_pac)
    return _stock_evp.get(game_pac, {})


def stock_2011(game_pac):
    """{number: name} of 2011's own entrances (main numbers below 10000)"""
    if game_pac in _stock:
        return _stock[game_pac]
    out = {}
    if game_pac:
        for f in sorted(os.listdir(os.path.join(game_pac, 'evt'))):
            if not re.match(r'(?i)nyujyo.\.pac$', f):
                continue
            ents = epk8_read(open(os.path.join(game_pac, 'evt', f), 'rb').read())
            for (g, n), raw in ents.items():
                if g.startswith('EVP') and n.isdigit() and int(n) < 10000:
                    u = unbpe(raw)
                    out[int(n)] = evp_name(u)
                    _stock_evp.setdefault(game_pac, {})[int(n)] = u
    _stock[game_pac] = out
    return out


def pack_numbers(mods):
    """entrance numbers the installed packs already add (pacentry lines)"""
    used = set()
    if mods and os.path.isdir(mods):
        for root, dirs, files in os.walk(mods):
            if 'PacOverlay' in root or os.path.exists(os.path.join(root, 'disabled')):
                continue
            if 'pack.txt' in files:
                for line in open(os.path.join(root, 'pack.txt'), encoding='latin-1'):
                    m = re.match(r'\s*pacentry\s+\S*nyujyo\S*\s+EV[PT]\S\s+(\d+)', line, re.I)
                    if m:
                        n = int(m.group(1))
                        used.add(n if n < 10000 else n % 1000)
    return used


def in_2011(name, game_pac):
    if not name:
        return None
    k = norm(name)
    for n, nm in stock_2011(game_pac).items():
        if norm(nm) == k:
            return n
    return None


# ============================================================================ games

GAMES = collections.OrderedDict([
    ('svr07', dict(name='SmackDown vs. Raw 2007', platform='x360', disc_hint='545107E0', disc_hints=['545107E0', 'SLES_544.89', 'SLUS_214.27'], status='ready')),
    ('svr08', dict(name='SmackDown vs. Raw 2008', platform='x360', disc_hint='5451080B', disc_hints=['5451080B', 'SLES_548.79', 'SLUS_216.45'], status='ready')),
    ('svr09', dict(name='SmackDown vs. Raw 2009', platform='x360', disc_hint='54510826', disc_hints=['54510826', 'SLES_552.51', 'SLUS_218.10'], status='ready')),
    ('svr10', dict(name='SmackDown vs. Raw 2010', platform='x360', disc_hint='54510844', disc_hints=['54510844', 'SLES_555.45', 'SLUS_219.01'], status='ready')),
    ('low', dict(name='WWE Legends of WrestleMania', platform='x360', disc_hint='54510838', status='ready')),
    ('wwe12', dict(name="WWE '12", platform='x360', disc_hint='54510890', status='ready')),
    ('wwe13', dict(name="WWE '13", platform='x360', disc_hint='545108B4', status='ready')),
    ('wwe2k14', dict(name='WWE 2K14', platform='x360', disc_hint='545408B2', status='ready')),
    ('wwe2k15', dict(name='WWE 2K15', platform='x360', disc_hint='545408B6', status='ready')),
    ('wwe2k16', dict(name='WWE 2K16', platform='x360', disc_hint='315A07D2', status='ready')),
    ('wwe2k17', dict(name='WWE 2K17', platform='x360', disc_hint='545408BA', status='ready')),
    ('hctp', dict(name='SmackDown! Here Comes the Pain', platform='ps2', disc_hint='SLES_520.36', disc_hints=['SLES_520.36', 'SLUS_207.87'], status='ready')),
    ('sym', dict(name='SmackDown! Shut Your Mouth', platform='ps2', disc_hint='SLES_512.83', disc_hints=['SLES_512.83', 'SLUS_204.83', 'SLUS_205.12'], status='ready')),
    ('jbi', dict(name='WWF SmackDown! Just Bring It', platform='ps2', disc_hint='SLES_504.77', disc_hints=['SLES_504.77', 'SLUS_203.16', 'SLUS_202.69', 'SLUS_201.22'], status='ready')),
    ('svr04', dict(name='SmackDown! vs. Raw', platform='ps2', disc_hint='SLES_527.81', disc_hints=['SLES_527.81', 'SLUS_210.60', 'SLUS_209.36'], status='ready')),
    ('svr06', dict(name='SmackDown! vs. Raw 2006', platform='ps2', disc_hint='SLES_536.76', disc_hints=['SLES_536.76', 'SLUS_212.86', 'SLUS_211.34'], status='ready')),
])
# (disc_hint: the Xbox 360 title id / PS2 serial discfs.open(iso).info() reports)


# ---- SvR 2008 (Xbox 360): pac/evt/Nyujyo0-3.pac, EPAC groups EVP1-4/EVT1-4, numbers 4000+

def only(number):
    """want for pac_entries: the one scene's script and tracks"""
    return lambda g, n: 1 if n.isdigit() and int(n) == number else 0


def scripts_only(g, n):
    """want for a list: the scripts unpacked, the track banks as stored (only their names are needed)"""
    return 1 if g.startswith('EVP') else 2 if g.startswith('EVT') else 0


def svr08_entrances(src, want=None):
    evp, evt = {}, {}
    for f in src.listdir('pac/evt'):
        if not re.match(scene_rx(), f):
            continue
        for (g, n), raw in pac_entries(src.read('pac/evt/' + f), want).items():
            if not n.isdigit():
                continue
            (evp if g.startswith('EVP') else evt if g.startswith('EVT') else {})[int(n)] = raw
    return evp, evt


def svr08_list(src, game_pac):
    evp, evt = svr08_entrances(src, scripts_only if VICTORY else None)
    out = []
    for n in sorted(evp):
        if n >= 10000 or n not in evt:
            continue
        try:
            e = E08.parse_evp(evp[n], '2008')
        except Exception as x:
            log('svr08 %d: EVP not readable (%s)' % (n, x))
            continue
        nm = e['name'].split(b'\0')[0].decode('latin-1', 'replace').strip()
        props = [o['model'] for o in e['objs'] if (o['t0'], o['t1']) == E08.TYPE_PROP]
        frames = {}
        try:
            for x in E08.read_evt(evt[n], '2008').entries:
                if x['id'] == 0 and x['y'] == 0 and x['x'] in (0, 5, 10, 15, 20):
                    frames[x['x'] // 5] = x['frames']
        except Exception:              # (a victory list keeps the banks as stored: no frames)
            pass
        out.append(dict(number=n, name=nm, segments=len(frames), frames=[frames.get(k, 0) for k in range(5)],
                        objects=len(e['objs']), props=props, has_pyro=any((o['t0'], o['t1']) == E08.TYPE_GLOBAL for o in e['objs']),
                        has_movie=False, has_theme=False, in_2011=in_2011(nm, game_pac)))
    return out


def svr08_convert(src, entrance, number, name, parts, rep):
    evp, evt = svr08_entrances(src, only(entrance))
    if entrance not in evp or entrance not in evt:
        raise InputError('SvR 2008 has no entrance %d' % entrance)
    progress(20, 'converting the script')
    log_lines = []
    e8 = E08.parse_evp(evp[entrance], '2008')
    e11 = E08.convert_evp(e8, number, log=log_lines)
    if name:
        nm = name.encode('latin-1', 'replace')[:29]
        e11['name'] = nm + bytes(30 - len(nm))
    progress(45, 'converting the tracks')
    t11 = E08.convert_evt(evt[entrance], log=log_lines)
    p11 = E08.build_evp(e11)
    rep['notes'] += log_lines
    rep['warnings'] += [l for l in log_lines if l.startswith('WARNING')]
    rep['dropped'] += [l for l in log_lines if l.startswith('dropped')]
    return p11, t11


# ---- HCTP (PS2): PAC/EVTENT.PAC (EVET banks + ENTR/SEVP), names PAC/MOVES.PAC

def hctp_src(src):
    ents = EH.dpac(src.read('PAC/EVTENT.PAC'))
    banks = {int(n): b for g, n, b in ents if g == 'EVET' and n.isdigit()}
    entr = {n: b for g, n, b in ents if g == 'ENTR'}
    names = {}
    if src.exists('PAC/MOVES.PAC'):
        for g, n, b in EH.dpac(src.read('PAC/MOVES.PAC')):
            if g in ('MOVE', 'MOVS') and n == 'LIST':     # (SvR 2004: MOVS/LIST, BPE-packed sub-entries)
                for i, sub in EH.pac(b):
                    if sub[:4] == b'BPE ':
                        sub = svrfmt.bpe_decode(sub)
                    if i == 2:
                        for k in range(struct.unpack_from('<H', sub, 0)[0]):
                            r = sub[2 + 32 * k:2 + 32 * (k + 1)]
                            names[struct.unpack_from('<H', r, 0)[0]] = r[2:32].split(b'\0')[0].decode('latin-1')
    return dict(banks=banks, sevp=entr['SEVP'], sydt=entr.get('SYDT')), names


def hctp_list(src, game_pac, game='hctp'):
    s, names = hctp_src(src)
    sevp = EH.parse_sevp(s['sevp'])
    out = []
    for n in sorted(sevp):
        bank = s['banks'].get(n)
        if not bank or len(bank) <= 512:
            continue                               # (placeholders)
        T, ents = EH.H.read_bank(bank)
        kinds = [EH.kind_of(o) for o in sevp[n]['objs']]
        frames = {}
        groups = EH.GAMES[game]['groups'] or [(k, k + 1) for k in range(5)]
        for o, k in zip(sevp[n]['objs'], kinds):
            if k == 'char':
                for j, (a, b) in enumerate(groups):
                    tot = 0
                    for sl in o['slots'][a:b]:
                        e = next((x for x in ents if (x['id'], x['x'], x['y']) == (sl['tid'], sl['tx'], sl['ty'])), None)
                        tot += e['frames'] if e else 0
                    if a < len(o['slots']):
                        frames[j] = tot
                break
        out.append(dict(number=n, name=names.get(n, ''), segments=len(frames), frames=[frames.get(k, 0) for k in range(5)],
                        objects=len(kinds), props=[o['model'] for o, k in zip(sevp[n]['objs'], kinds) if k == 'prop'],
                        has_pyro=False, has_movie=False, has_theme=False,
                        in_2011=None))        # (HCTP's are its own motions; 2011 never shipped them)
    return out


def hand_library(game_pac):
    """the 2011 hand-pose library for the HCTP-path converters' finger tracks (hand_tracks.py)"""
    import hand_tracks
    EH.DEFAULT_HAND_LIB = hand_tracks.library(game_pac) if game_pac else {}
    return EH.DEFAULT_HAND_LIB


def hctp_convert(src, entrance, number, name, parts, rep, game='hctp', game_pac=None):
    hand_library(game_pac)
    s, names = hctp_src(src)
    if entrance not in EH.parse_sevp(s['sevp']) or len(s['banks'].get(entrance, b'')) <= 512:
        raise InputError('%s has no entrance %d' % (GAMES[game]['name'], entrance))
    progress(20, 'converting the script and tracks')
    own = {}
    if game == 'hctp' and game_pac:   # (the bike's own model: entrance_props / hctp_bike; else 2011's 2540)
        import hctp_bike
        uses = {o['model'] for o in EH.parse_sevp(s['sevp'])[entrance]['objs'] if EH.kind_of(o) == 'prop'}
        for (g, model), pid in EPR.PORTED.items():
            if g != 'hctp' or model not in uses:
                continue
            try:
                data, bike_notes = hctp_bike.build(src.read('PAC/BG.PAC'), game_pac)
            except Exception as x:
                rep['notes'].append('prop %d: its own model not built (%s): 2011\'s 2540 with its nodes mapped' % (model, x))
                continue
            own[model] = pid
            PROPS.append((pid, EH.VEHICLES[model]['model'], data))
            rep['notes'] += bike_notes + ['prop %d: HCTP\'s own as evtobj %d, added by the pack (2011\'s %d if it can\'t '
                                          'be: propfallback)' % (model, pid, EH.VEHICLES[model]['model'])]
    evp, evt, r, notes = EH.build(entrance, number, s, name or names.get(entrance, '%s %d' % (game.upper(), entrance)),
                                  game=game, own_props=own)
    rep['notes'] += notes + ['%s: %d' % kv for kv in sorted(r.items())]
    rep['dropped'] += ['%s: %d' % kv for kv in sorted(r.items()) if 'dropped' in kv[0]]
    return evp, evt


# ---- SvR 2010 (Xbox 360): pac/evt/Nyujyo0-3.pac, EPK8 groups EVP1-4/EVT1-4, numbers 100-840

def svr04_list(src, game_pac):
    return hctp_list(src, game_pac, 'svr04')


def svr04_convert(src, entrance, number, name, parts, rep, game_pac=None):
    return hctp_convert(src, entrance, number, name, parts, rep, 'svr04', game_pac)


# ---- SmackDown! vs. Raw 2006 (PS2): tools/svr06_entrance.py (PAC/EVT.PAC; 2007's script layout in
#      little-endian, HCTP-style bodies re-encoded); about two thirds also exist in SvR 2007 (in_svr07)

# ---- SmackDown! Shut Your Mouth (PS2): tools/sym_entrance.py (PAC/EVTENT.PAC, SYDT scripts, halfword-framed
#      records, slot-relative roots baked; HCTP's back end). Numbers 3000-3113 overlap HCTP's (other entrances).

def sym_list(src, game_pac):
    import sym_entrance
    return sym_entrance.list_entrances(src)


def sym_convert(src, entrance, number, name, parts, rep, game_pac=None):
    import sym_entrance
    hand_library(game_pac)
    try:
        return sym_entrance.convert(src, entrance, number, name, rep)
    except ValueError as x:
        raise InputError(str(x))


# ---- WWF SmackDown! Just Bring It (PS2): tools/jbi_entrance.py (EN3.PAC, CSV scripts; SYM's motion codec,
#      fed through sym_entrance). Numbers 0-78 and 1000-1268; names only in the ELF ("JBI <n>").

def jbi_list(src, game_pac):
    import jbi_entrance
    return jbi_entrance.list_entrances(src)


def jbi_convert(src, entrance, number, name, parts, rep, game_pac=None):
    import jbi_entrance
    hand_library(game_pac)
    try:
        if not name:
            rep['name'] = jbi_entrance.read_source(src).get('names', {}).get(entrance, '')
        return jbi_entrance.convert(src, entrance, number, name, rep)
    except ValueError as x:
        raise InputError(str(x))


def svr06_list(src, game_pac):
    import svr06_entrance
    out = svr06_entrance.list_entrances(src, game_pac=game_pac)
    for r in out:
        # (a few 2006 script names run words together: "TheRock" -> "The Rock")
        r['name'] = re.sub(r'(?<=[a-z])(?=[A-Z])', ' ', r['name'])
        if r.get('in_2011') is None:
            r['in_2011'] = in_2011(r['name'], game_pac)
    return out


def svr06_convert(src, entrance, number, name, parts, rep, game_pac=None):
    import svr06_entrance
    try:
        return svr06_entrance.convert(src, entrance, number, name, rep, game_pac=game_pac)
    except ValueError as x:
        raise InputError(str(x))


def svr10_entrances(src, want=None):
    evp, evt = {}, {}
    for f in src.listdir('pac/evt'):
        if not re.match(scene_rx(), f):
            continue
        for (g, n), raw in pac_entries(src.read('pac/evt/' + f), want).items():
            if n.isdigit() and int(n) < 10000:
                (evp if g.startswith('EVP') else evt if g.startswith('EVT') else {})[int(n)] = raw
    return evp, evt


def svr10_list(src, game_pac):
    evp, evt = svr10_entrances(src, scripts_only if VICTORY else None)
    out = []
    for n in sorted(evp):
        if n not in evt:
            continue
        try:
            e = E08.parse_evp(evp[n], '2011')
        except Exception as x:
            log('svr10 %d: EVP not readable (%s)' % (n, x))
            continue
        nm = evp_name(evp[n])
        frames = {}
        try:
            for x in ymks.read_bank(evt[n]).entries:
                if x['id'] == 0 and x['y'] == 0 and x['x'] in (0, 5, 10, 15, 20):
                    frames[x['x'] // 5] = x['frames']
        except Exception:
            pass
        out.append(dict(number=n, name=nm, segments=len(frames), frames=[frames.get(k, 0) for k in range(5)],
                        objects=len(e['objs']), props=[o['model'] for o in e['objs'] if (o['t0'], o['t1']) == E08.TYPE_PROP],
                        has_pyro=any((o['t0'], o['t1']) == E08.TYPE_GLOBAL for o in e['objs']),
                        has_movie=False, has_theme=False, in_2011=in_2011(nm, game_pac)))
    return out


def svr10_convert(src, entrance, number, name, parts, rep, game_pac=None):
    evp, evt = svr10_entrances(src, only(entrance))
    if entrance not in evp or entrance not in evt:
        raise InputError('SvR 2010 has no entrance %d' % entrance)
    progress(20, 'converting the script')
    stock = stock_2011_evps(game_pac)
    if not stock:
        rep['warnings'].append('2011 pac folder not found: object 6 position templates and object 0 model rule skipped')
    m0 = set()
    for b in stock.values():
        try:
            m0.add(E08.parse_evp(b, '2011')['objs'][0]['model'])
        except Exception:
            pass
    lines = []
    e11 = E10.convert_evp(E08.parse_evp(evp[entrance], '2011'), number, m0, E10.obj6_templates(stock), lines)
    if name:
        nm = name.encode('latin-1', 'replace')[:29]
        e11['name'] = nm + bytes(30 - len(nm))
    progress(45, 'converting the tracks')
    t11 = E10.convert_evt(evt[entrance], lines)
    missing = [o['model'] for o in e11['objs'] if (o['t0'], o['t1']) == E08.TYPE_PROP and o['model'] in (2403, 2405)]
    if missing:
        rep['warnings'].append('props %s are not in 2011 (they will not show)' % missing)
    rep['notes'] += lines
    rep['dropped'] += [l for l in lines if l.startswith('dropped')]
    return E08.build_evp(e11), t11


# ---- WWE '13 (Xbox 360): pac/evt/Entrance00-08.pac + Entrance_dlc01.pac, xcompressed (svrmod xdec),
#      EPK8 groups 'EVP ' / 'EVZ ' (YANM motion records), numbers 50-856; tools/wwe13_entrance.py
# ---- WWE '12: pac/evt/Nyujyo0-9,A.pac (plain EPK8, EVP?/EVZ?), FK bodies; WWE 2K14: Entrance00-08.pac
#      (xcompressed EPAC): the same converter with --game (wwe13_entrance.GAMES)

SVRMOD = None          # --svrmod (else found next to the tools / the Mod Maker)
_w13 = {}


def wwe13_entrances(src, game='wwe13'):
    key = (id(src), game)
    if key not in _w13:
        try:
            sv = E13.find_svrmod(SVRMOD)
        except E13.ToolMissing as x:
            if SVRMOD:
                raise InputError(str(x))
            sv = None                     # (only needed for xcompressed pacs: read_source says so)
        try:
            _w13[key] = E13.read_source(src, sv, game)
        except (FileNotFoundError, E13.ToolMissing) as x:
            raise InputError(str(x))
    return _w13[key]


def w13_list(game):
    def lister(src, game_pac):
        evp, evz = wwe13_entrances(src, game)
        return E13.entry_list(evp, evz, game_pac)
    return lister


def w13_convert(game):
    def conv(src, entrance, number, name, parts, rep, game_pac=None):
        return wwe13_convert(src, entrance, number, name, parts, rep, game_pac, game)
    return conv


wwe13_list = w13_list('wwe13')


PROPS = []   # (this conversion's own props: [(id, game prop id, entry bytes)] - entrance_props.write)


def own_vehicles(src, game, evp11, game_pac, rep):
    """the source game's own vehicle in place of 2011's (entrance_props.PORTED: same model, its textures) when
    the script uses it: a new evtobj id added by the pack, 2011's prop as the fallback"""
    used = {o['model'] for o in E08.parse_evp(evp11, '2011')['objs'] if E13.kind(o) == 'prop'}
    for (g, model), pid in EPR.PORTED.items():
        if g != game or model not in used or not game_pac:
            continue
        try:
            raw = src.read('pac/evt/evtobj.pac')
            if raw[:4] not in (b'EPAC', b'EPK8'):
                raw = E13.xdec(raw, E13.find_svrmod(SVRMOD))
            theirs = E13.epk8_entries(raw).get(('SOBJ', str(model)))
            ours = EPR.game_prop(game_pac, model)
            data = EPR.retextured(ours, theirs) if theirs and ours else None
        except Exception as x:
            rep['notes'].append('prop %d: the source\'s own not read (%s): 2011\'s kept' % (model, x))
            continue
        if data is None:
            rep['notes'].append('prop %d: the source\'s is 2011\'s own (kept)' % model)
            continue
        PROPS.append((pid, model, data))
        evp11 = EPR.set_prop(evp11, model, pid)
        rep['notes'].append('prop %d: %s\'s own (its textures) as evtobj %d, added by the pack (2011\'s %d if it '
                            'can\'t be: propfallback)' % (model, E13.GAMES[game]['name'], pid, model))
    return evp11


DLC_PROP_FALLBACK = 2006   # (a prop every 2011 install has: only if the pack can't add the entrance's own)


def dlc_props(src, game, evp11, rep):
    """the props of a DLC entrance that 2011's evtobj lacks, from the game's packages (evtobj_dlc<nn>.pac, 2K16's pac/prop/<id>_dlc<nn>.pac):
    the model converted (svrmod wwe13prop), added by the pack under its own id"""
    packages = getattr(getattr(src, 'dlc', None), 'packages', None)
    if not packages:
        return
    missing = collections.defaultdict(list)
    for w in rep['warnings']:
        m = re.search(r"prop (\d+) is not in 2011's evtobj", w)
        if m:
            missing[int(m.group(1))].append(w)
    if not missing:
        return
    found = {}
    for pk in packages:
        for low in pk.matching(r'^pac/(evt/evtobj_dlc\d+|prop/\d+_dlc\d+)\.pac$'):
            raw = pk.read(low)
            if raw[:4] == b'\x0f\xf5\x12\xed':
                raw = E13.xdec(raw, E13.find_svrmod(SVRMOD))
            for (g, n), b in E13.epk8_entries(raw).items():
                if g == 'SOBJ' and n.isdigit():
                    found.setdefault(int(n), b)
    for pid, warnings in sorted(missing.items()):
        if pid not in found or any(p[0] == pid for p in PROPS):
            continue
        with tempfile.TemporaryDirectory() as d:
            a, b = os.path.join(d, 'in.bin'), os.path.join(d, 'out.bin')
            with open(a, 'wb') as f:
                f.write(found[pid])
            r = subprocess.run([E13.find_svrmod(SVRMOD), 'wwe13prop', a, b], capture_output=True, text=True, stdin=subprocess.DEVNULL)
            if r.returncode or not os.path.exists(b):
                rep['notes'].append('prop %d: its DLC model not converted (%s)' % (pid, (r.stdout or '').strip()[:120]))
                continue
            with open(b, 'rb') as f:
                PROPS.append((pid, DLC_PROP_FALLBACK, f.read()))
        rep['warnings'] = [w for w in rep['warnings'] if w not in warnings]
        rep['notes'].append('prop %d: the DLC package\'s own model as evtobj %d, added by the pack' % (pid, pid))


def wwe13_convert(src, entrance, number, name, parts, rep, game_pac=None, game='wwe13'):
    evp, evz = wwe13_entrances(src, game)
    if entrance not in evp or entrance not in evz:
        raise InputError("%s has no entrance %d" % (E13.GAMES[game]['name'], entrance))
    if not game_pac:
        rep['warnings'].append('2011 pac folder not found: prop tracks, the lights object and the model checks are skipped')
    progress(15, 'converting the script and tracks')

    def prog(done, total, kind):
        progress(15 + int(40 * done / total), 'tracks %d/%d (%s)' % (done, total, kind))
    evp11, evt11, r = E13.convert(evp[entrance], evz[entrance], number, game_pac, name, progress=prog, game=game)
    for k in ('notes', 'warnings', 'dropped'):
        rep[k] += r[k]
    evp11 = own_vehicles(src, game, evp11, game_pac, rep)
    dlc_props(src, game, evp11, rep)
    rep['notes'].append('not converted: theme, face / finger tracks; titantron only with --titantron (Bink 2: a decoder the player installs)')
    return evp11, evt11


# ---- SvR 2007 (Xbox 360): pac/evt/evt1.pac + evt2.pac (EPAC, EVP1/EVT1, EVP2/EVT2), 4000-4362
#      (evt0 = the entrance editor and props, evt3 CAE parts, evt4-8 season / win scenes)
# ---- SvR 2009 (Xbox 360): pac/evt/Nyujyo0-3.pac (EPAC, EVP1-4/EVT1-4), 4002-4424 (TNyujyo = tests)
# Both use 2008's script layout (28-byte objects) and its headerless track bank.

def old_entrances(src, files, want=None):
    evp, evt = {}, {}
    if VICTORY:
        files = (scene_rx(),)
    for f in src.listdir('pac/evt'):
        if not any(re.match(rx, f, re.I) for rx in files):
            continue
        for d in (src.reads('pac/evt/' + f) if hasattr(src, 'reads') else [src.read('pac/evt/' + f)]):
            for (g, n), raw in pac_entries(d, want).items():
                if n.isdigit() and int(n) < 9999:
                    (evp if g.startswith('EVP') else evt if g.startswith('EVT') else {})[int(n)] = raw
    return evp, evt


SRC07 = (r'evt[12]\.pac$',)
SRC09 = (r'nyujyo[0-9a-z]\.pac$',)   # (its DLC: NyujyoD.pac)


def old_list(files):
    def lister(src, game_pac):
        evp, evt = old_entrances(src, files, scripts_only if VICTORY else None)
        out = []
        for n in sorted(evp):
            if n not in evt:
                continue
            try:
                e = E08.parse_evp(evp[n], '2008')
            except Exception as x:
                log('%d: EVP not readable (%s)' % (n, x))
                continue
            nm = evp_name(evp[n])
            frames = {}
            try:
                for x in E08.read_evt(evt[n], '2008').entries:
                    if x['id'] == 0 and x['y'] == 0 and x['x'] in (0, 5, 10, 15, 20):
                        frames[x['x'] // 5] = x['frames']
            except Exception:
                pass
            out.append(dict(number=n, name=nm, segments=len(frames), frames=[frames.get(k, 0) for k in range(5)],
                            objects=len(e['objs']),
                            props=[o['model'] for o in e['objs'] if (o['t0'], o['t1']) == E08.TYPE_PROP],
                            has_pyro=any((o['t0'], o['t1']) == E08.TYPE_GLOBAL for o in e['objs']),
                            has_movie=False, has_theme=False, in_2011=in_2011(nm, game_pac)))
        return out
    return lister


def drop_ops(evt11, op, ys, rep, label):
    """remove every op `op` event from the tracks whose y is in ys (2011 bank bytes in and out)"""
    b = ymks.read_bank(evt11)
    gone = 0
    for e in b.entries:
        if not e['data'] or e.get('share') or e['y'] not in ys:
            continue
        st = ymks.parse_stream(e['data'])
        items, changed = [], False
        for it in st.items:
            if it[0] == 'rec':
                ev, pose = ymks.split_record(it[1])
                if ev:
                    lst, ok = ymks.parse_events(ev, b.T)
                    if ok and any(o == op for o, _ in lst):
                        keep = b''.join(bb for o, bb in lst if o != op)
                        gone += sum(1 for o, _ in lst if o == op)
                        rec = ymks.join_record(keep, pose)
                        items.append(('rec', rec, len(rec) > 0x9f))
                        changed = True
                        continue
            items.append(it)
        if changed:
            e['data'] = ymks.build_stream(ymks.Stream(items))
    for i, e in enumerate(b.entries):
        if e.get('share') and i:
            e['data'] = b.entries[i - 1]['data']
    if gone:
        rep['dropped'].append('%s: %d op-%d events removed' % (label, gone, op))
    return ymks.write_bank(b)


def set_name(e11, name):
    if name:
        nm = name.encode('latin-1', 'replace')[:29]
        e11['name'] = nm + bytes(30 - len(nm))


def svr07_convert(src, entrance, number, name, parts, rep):
    evp, evt = old_entrances(src, SRC07)
    if entrance not in evp or entrance not in evt:
        raise InputError('SvR 2007 has no entrance %d' % entrance)
    progress(20, 'converting the script')
    lines = []
    e7 = E08.parse_evp(evp[entrance], '2008')
    for i, o in enumerate(e7['objs']):      # (Yuke's own 2007 -> 2011 pair 4252 -> 772: extras are 13902)
        if i and (o['t0'], o['t1']) == E08.TYPE_CHAR and o['model'] != E08.GENERIC_CHAR_08:
            lines.append('object %d: extra character model %d -> generic' % (i, o['model']))
            o['model'] = E08.GENERIC_CHAR_08
    e11 = E08.convert_evp(e7, number, log=lines)
    if e11['tail'] and any(r[0] == 9 for r in e11['tail']['recs']):
        lines.append('tail: the 2007-only type-9 record (a music cue) dropped')
        e11['tail']['recs'] = [r for r in e11['tail']['recs'] if r[0] != 9]
    set_name(e11, name)
    progress(45, 'converting the tracks')
    t11 = E08.convert_evt(evt[entrance], log=lines)
    t11 = drop_ops(t11, 79, (1, 2, 3), rep, 'face/hand tracks (2007 loop marker)')
    missing = [o['model'] for o in e11['objs'] if (o['t0'], o['t1']) == E08.TYPE_PROP
               and o['model'] in (2040, 2054, 2075, 2078, 2079, 2080, 3001)]
    if missing:
        rep['warnings'].append('props %s are not in 2011 (they will not show)' % missing)
    rep['notes'] += lines
    rep['dropped'] += [l for l in lines if l.startswith('dropped')]
    return E08.build_evp(e11), t11


def svr09_convert(src, entrance, number, name, parts, rep, game_pac=None, low=False):
    evp, evt = old_entrances(src, SRC09, only(entrance))
    if entrance not in evp or entrance not in evt:
        raise InputError('%s has no entrance %d' % ('Legends of WrestleMania' if low else 'SvR 2009', entrance))
    progress(20, 'converting the script')
    stock = stock_2011_evps(game_pac)
    if not stock:
        rep['warnings'].append('2011 pac folder not found: object 6 position templates and object 0 model rule skipped')
    m0 = set()
    for b in stock.values():
        try:
            m0.add(E08.parse_evp(b, '2011')['objs'][0]['model'])
        except Exception:
            pass
    lines = []
    e9 = E08.parse_evp(evp[entrance], '2008')        # (2008's object entries, 2011's model ids)
    gone = None
    if low:
        e9, gone = low_prepare(e9, lines)
    e11 = E10.convert_evp(e9, number, m0, E10.obj6_templates(stock), lines)
    if e11['objs'] and e11['objs'][0]['model'] == E08.PREVIEW_CHAR_11:
        for o in e11['objs'][1:7]:                    # (Yuke's: extras follow the preview body)
            if (o['t0'], o['t1']) == E08.TYPE_CHAR and o['model'] == E08.GENERIC_CHAR_11:
                o['model'] = E08.PREVIEW_CHAR_11
    set_name(e11, name)
    progress(45, 'converting the tracks')
    t11 = E08.convert_evt(evt[entrance], log=lines)
    if gone is not None:                              # (the dropped object's tracks)
        b = ymks.read_bank(t11)
        b.entries = [x for x in b.entries if x['id'] != 25000 + gone]
        t11 = ymks.write_bank(b)
    missing = [o['model'] for o in e11['objs'] if (o['t0'], o['t1']) == E08.TYPE_PROP and o['model'] in
               (2116, 2152, 2262) + (LOW_MISSING_PROPS if low else ())]
    if missing:
        rep['warnings'].append('props %s are not in 2011 (they will not show)' % missing)
    rep['notes'] += lines
    rep['dropped'] += [l for l in lines if l.startswith('dropped')]
    return E08.build_evp(e11), t11


# ---- WWE Legends of WrestleMania (Xbox 360, 2009): SvR 2009's entrances (pac/evt/Nyujyo0-3.pac EPAC, 4001-4359,
#      singles 4000 + id - 100; 4170-4186 / 4230-4233 / 4350-4359 = "Gondola" ring-cart entrances) -> svr09_convert
#      plus: LoW's own generic extras -> 13902, object 6's positions always from 2011's template, the inert
#      trailing ring-cart prop dropped (a Gondola entrance's cart kept: LoW's own model, low_convert).
#      Titantrons are movies/titantron/<superstar id>.bik (--titantron <id>).

LOW_GENERIC = {10902, 24802, 24902}
LOW_MAYBE_GENERIC = {11902, 15902}        # (generic, except a tag partner at object 4)
LOW_CARTS = {2552, 2565}
LOW_CART_FALLBACK = 2526   # (2011's jeep, when the pack can't add the cart)
LOW_MISSING_PROPS = (2554, 2555, 2557, 2562, 2563, 2564, 2568, 2569)


def low_prepare(e, lines):
    """LoW script (2008 layout dict) -> (the same with LoW's differences from 2009 undone, index of a dropped object)"""
    e = dict(e)
    objs = [dict(o) for o in e['objs']]
    for i, o in enumerate(objs):
        if (o['t0'], o['t1']) != E08.TYPE_CHAR or i == 0:
            continue
        if o['model'] in LOW_GENERIC or (o['model'] in LOW_MAYBE_GENERIC and i != 4):
            lines.append('object %d: LoW generic extra %d -> %d' % (i, o['model'], E08.GENERIC_CHAR_11))
            o['model'] = E08.GENERIC_CHAR_11
        if i == 6 and o['nslots'] == 21:          # (2011 fills these from its own template)
            slots = list(o['slots'])
            for k in (0, 5, 10, 15, 20):
                sl = bytearray(slots[k])
                sl[E10.POS0:E10.POS1] = bytes(E10.POS1 - E10.POS0)
                slots[k] = bytes(sl)
            o['slots'] = slots
    gone = None
    if (objs and (objs[-1]['t0'], objs[-1]['t1']) == E08.TYPE_PROP and objs[-1]['model'] in LOW_CARTS
            and not any(E08.slot_fields(s)['on'] for s in objs[-1]['slots'])):   # (a Gondola entrance rides it)
        gone = len(objs) - 1
        lines.append('object %d: the unused ring-cart prop %d dropped' % (gone, objs[-1]['model']))
        objs.pop()
    e['objs'] = objs
    return e, gone


def low_convert(src, entrance, number, name, parts, rep, game_pac=None):
    evp11, evt11 = svr09_convert(src, entrance, number, name, parts, rep, game_pac, low=True)
    # the "Gondola" ring carts (4170-4186, 4230-4233, 4350-4359): LoW's own cart model (pac/evt/_EvtObj.pac SOBJ
    # 2552 / 2565: PACH {0: JBOY, 10: textures}, as 2011's props) as a new evtobj entry; the cart rides its own
    # world track, as 2011's vehicles do, the rider his
    uses = {o['model'] for o in E08.parse_evp(evp11, '2011')['objs'] if (o['t0'], o['t1']) == E08.TYPE_PROP}
    for model in sorted(uses & LOW_CARTS):
        pid = EPR.PORTED[('low', model)]
        try:
            f = next(n for n in src.listdir('pac/evt') if n.lower() == '_evtobj.pac')
            _, groups, _ = svrfmt.epac_read(src.read('pac/evt/' + f))
            data = next(EPR._unpack(b) for ty, es in groups for n, b in es
                        if ty == b'SOBJ' and n.decode('latin-1').strip(' \0') == str(model))
            data = svrfmt.pach_write([(k, EPR._unpack(v)) for k, v in svrfmt.pach_read(data)])   # (plain parts)
        except (OSError, StopIteration, ValueError) as x:
            rep['warnings'].append('ring cart %d: LoW\'s model not read (%s): it will not show' % (model, x))
            continue
        PROPS.append((pid, LOW_CART_FALLBACK, data))
        evp11 = EPR.set_prop(evp11, model, pid)
        rep['notes'].append('ring cart %d: LoW\'s own model as evtobj %d, added by the pack (2011\'s %d if it can\'t '
                            'be: propfallback)' % (model, pid, LOW_CART_FALLBACK))
    rep['notes'].append('titantron: LoW names them by superstar id (movies/titantron/<id>.bik): use --titantron <id>')
    return evp11, evt11


LISTERS = {'svr07': old_list(SRC07), 'svr09': old_list(SRC09), 'low': old_list(SRC09), 'svr08': svr08_list, 'svr10': svr10_list, 'hctp': hctp_list, 'svr04': svr04_list, 'svr06': svr06_list, 'sym': sym_list, 'jbi': jbi_list, 'wwe13': wwe13_list,
           'wwe12': w13_list('wwe12'), 'wwe2k14': w13_list('wwe2k14'), 'wwe2k15': w13_list('wwe2k15'),
           'wwe2k16': w13_list('wwe2k16'), 'wwe2k17': w13_list('wwe2k17')}
CONVERTERS = {'svr07': svr07_convert, 'svr09': svr09_convert, 'low': low_convert, 'svr08': svr08_convert, 'svr10': svr10_convert, 'hctp': hctp_convert, 'svr04': svr04_convert, 'svr06': svr06_convert, 'sym': sym_convert, 'jbi': jbi_convert, 'wwe13': wwe13_convert,
              'wwe12': w13_convert('wwe12'), 'wwe2k14': w13_convert('wwe2k14'), 'wwe2k15': w13_convert('wwe2k15'),
              'wwe2k16': w13_convert('wwe2k16'), 'wwe2k17': w13_convert('wwe2k17')}


# ============================================================================ commands

def opt(a, k, d=None):
    return a[a.index(k) + 1] if k in a and a.index(k) + 1 < len(a) else d


def cmd_games(a):
    # (disc_hints: every disc that game id reads - the 360 title id and, for SvR 2007-2010, the PS2 serial)
    return [dict(id=k, name=v['name'], platform=v['platform'], disc_hint=v['disc_hint'],
                 disc_hints=v.get('disc_hints', [v['disc_hint']]), status=v['status'])
            for k, v in GAMES.items()]


def game_of(a, lone=None):
    g = opt(a, '--game') or lone
    if g not in GAMES:
        raise InputError('unknown --game %r (games: %s)' % (g, ', '.join(GAMES)))
    if g not in LISTERS:
        raise InputError('%s entrances are not converted yet (research): docs/OTHER_GAMES_ENTRANCES.md' % GAMES[g]['name'])
    return g


def source_of(a):
    """-> (source, game, lone info or None): --src a disc / folder (--game needed) or one entrance file (--game
    optional: detected from the format)"""
    path = opt(a, '--src')
    if is_lone_file(path):
        ent = opt(a, '--entrance')
        src, g, found = lone_source(path, opt(a, '--game'), int(ent) if ent and ent.isdigit() else None)
        g = game_of(a, g)
        info = dict(file=os.path.basename(path), detected=found, limits=lone_limits(g))
        if g != found:
            info['warning'] = 'the file looks like %s, read as %s (--game)' % (GAMES[found]['name'], GAMES[g]['name'])
        return src, g, info
    g = game_of(a)
    return open_source(path), g, None


def cmd_detect(a):
    """--src <one entrance file> -> {game, name, platform, entrances, limits[]} (what list / convert will use)"""
    path = opt(a, '--src')
    if not is_lone_file(path):
        raise InputError('detect takes one entrance file (EPAC / EPK8 / DPAC / xcompressed pac): %s' % path)
    src, g, info = source_of(a)
    try:
        n = len(LISTERS[g](src, None))
    except InputError:
        raise
    except Exception as x:
        raise InputError('%s: read as %s, but its entrances are not readable (%s)' % (info['file'], GAMES[g]['name'], x))
    out = dict(ok=True, game=g, name=GAMES[g]['name'], platform=GAMES[g]['platform'], entrances=n, **info)
    if g == 'wwe2k16':
        out['note'] = 'WWE 2K16 and 2K17 EPAC entrance pacs are the same format (converted the same way)'
    return out


def kind_of(a, g):
    global VICTORY
    VICTORY = opt(a, '--kind', 'entrance') == 'victory'
    if VICTORY and g not in VICTORY_GAMES:
        raise InputError('%s victory scenes are not converted (SvR 2008, 2009, 2010 and Legends of WrestleMania are)'
                         % GAMES[g]['name'])


def cmd_list(a):
    src, g, lone = source_of(a)
    kind_of(a, g)
    progress(5, 'reading entrances')
    out = LISTERS[g](src, find_game_pac(opt(a, '--game-pac')))
    if VICTORY:                        # (singles: a mod's victory= replaces those; tag, title, match-type stay)
        out = [r for r in out if r['number'] < 7150]
    for r in out:                      # (the Mod Maker contract's item key: what convert --entrance takes)
        r.setdefault('key', str(r['number']))
    progress(100, '%d entrances' % len(out))
    return out


def free_numbers(game_pac, mods):
    used = set(stock_2011(game_pac)) | pack_numbers(mods)
    return [n for lo, hi, f, gl in NYUJYO for n in range(lo, hi + 1) if n not in used]


def cmd_free(a):
    gp = find_game_pac(opt(a, '--game-pac'))
    if not gp:
        raise InputError('the 2011 pac folder was not found: give --game-pac')
    mods = opt(a, '--mods') or os.path.join(os.path.dirname(gp), 'Mods')
    if opt(a, '--kind') == 'victory':     # (6300-6999: past the superstars' own 6000 + id)
        used = set(stock_victories(gp)) | pack_victories(mods)
        return [n for n in range(6300, 7000) if n not in used]
    return free_numbers(gp, mods)


PYRO_GAMES = ('svr10', 'wwe12', 'wwe13', 'wwe2k14', 'wwe2k15', 'wwe2k16', 'wwe2k17')


def pyro(src, g, entrance, number, d, rep):
    """the source entrance's PCE6/LPE6 particle set -> pack d/pyro + pacentry lines (notes only on failure)"""
    if g not in PYRO_GAMES:
        if g in ('svr07', 'svr09', 'svr06'):
            rep['notes'].append('pyro: %s particle sets are an older format (not converted)' % GAMES[g]['name'])
        return
    try:
        if g in ('wwe2k16', 'wwe2k17'):                # (one particle pac per entrance)
            fn = 'pac/particle/pce6%04d.pac' % entrance
            raw = src.read(fn) if src.exists(fn) else b''
            if not raw:
                rep['notes'].append('pyro: the source has no %s' % fn)
                return
            if raw[:4] not in (b'EPAC', b'EPK8'):
                raw = E13.xdec(raw, E13.find_svrmod(SVRMOD))
            ents = E13.epk8_entries(raw)
            pce = next((b for (gg, n), b in ents.items() if gg == 'PCE6'), None)
            if pce is None:
                rep['notes'].append('pyro: %s has no PCE6 set' % fn)
                return
            ents = {('PCE6', '%04d' % entrance): pce}
        else:
            raw = src.read('pac/prtcls.pac')
            if raw[:4] not in (b'EPAC', b'EPK8'):
                raw = E13.xdec(raw, E13.find_svrmod(SVRMOD))
            ents = E13.epk8_entries(raw)
    except Exception as x:
        rep['notes'].append('pyro: pac/prtcls.pac not readable (%s)' % x)
        return
    name = '%04d' % entrance
    pce = ents.get(('PCE6', name))
    if pce is None:
        rep['notes'].append('pyro: the source has no particle set for entrance %d' % entrance)
        return
    lpe = ents.get(('LPE6', name), pce)
    os.makedirs(os.path.join(d, 'pyro'), exist_ok=True)
    lines = []
    for grp, b in (('PCE6', pce), ('LPE6', lpe)):
        fn = 'pyro/%s_%04d.bin' % (grp, number)
        open(os.path.join(d, fn), 'wb').write(b)
        lines.append('pacentry prtcls.pac %s %04d %s' % (grp, number, fn))
    with open(os.path.join(d, 'pack.txt'), 'a', newline='\r\n') as f:
        f.write('\n'.join(lines) + '\n')
    rep['notes'].append('pyro: prtcls.pac PCE6/LPE6 %s -> %04d%s' % (name, number, '' if ('LPE6', name) in ents else ' (LPE6 from PCE6)'))


TITANTRON_DIRS = ('movies/titantron', 'MOVIES/TITANTRON', 'movies/Titantron')


def titantron(src, a, entrance, out, rep):
    """--titantron: the source game's titantron -> <out>/movie.bik or movie.mp4 (notes only on failure)"""
    v = opt(a, '--titantron')
    n = int(v) if v and v.isdigit() else entrance
    found = None
    # (2K15+: ent_<id>_<variant>_0.bk2, the PRO's movie being id * 100 + variant)
    ent = ('ent_%04d_%d_0' % (n // 100, n % 100)) if n >= 10000 else None
    for d in TITANTRON_DIRS:
        try:
            names = src.listdir(d)
        except Exception:
            continue
        for f in names:
            stem, ext = os.path.splitext(f)
            if (stem.isdigit() and int(stem) == n or stem.lower() == ent) and ext.lower() in ('.bik', '.bik2', '.bk2'):
                found = d + '/' + f
                break
        if found:
            break
    if not found:
        rep['notes'].append('titantron: no movies/titantron/%03d.* in the source' % n)
        return
    raw = src.read(found)
    if raw[:3] == b'BIK':                                   # Bink 1: the game plays it as it is
        open(os.path.join(out, 'movie.bik'), 'wb').write(raw)
        rep['files'].append('movie.bik')
        rep['notes'].append('titantron: %s -> movie.bik (movie=movie.bik)' % found)
        return
    try:
        import bink2_convert                                # (optional: not every package ships it)
    except ImportError:
        rep['notes'].append('titantron: %s is Bink 2 and tools/bink2_convert.py is missing' % found)
        return
    tmp = os.path.join(out, 'titantron_src.bk2')
    open(tmp, 'wb').write(raw)
    try:
        progress(95, 'titantron (Bink 2)')
        code, res = bink2_convert.convert(tmp, os.path.join(out, 'movie.mp4'), opt(a, '--bink2-tool'))
    finally:
        try:
            os.remove(tmp)
        except OSError:
            pass
    rep['notes'] += ['titantron: %s' % w for w in res.get('warnings', [])]
    if code == 0 and res.get('ok'):
        rep['files'].append('movie.mp4')
        rep['notes'].append('titantron: %s -> movie.mp4 (movie=movie.mp4; a .bik fallback is made at import / install)' % found)
    else:
        rep['notes'].append('titantron: not converted: %s' % res.get('error', 'Bink 2 decoder failed'))


def cmd_convert(a):
    src, g, lone = source_of(a)
    kind_of(a, g)
    if VICTORY:
        return convert_victory(a, src, g, lone)
    try:
        entrance, number = int(opt(a, '--entrance')), int(opt(a, '--number'))
    except (TypeError, ValueError):
        raise InputError('--entrance and --number must be numbers')
    out = opt(a, '--out')
    if not out:
        raise InputError('--out is needed')
    if not 100 <= number < 1000:
        raise InputError('--number must be 100..999 (Create-An-Entrance parts are 10000 + 1000k + number)')
    gp = find_game_pac(opt(a, '--game-pac'))
    if gp and number in stock_2011(gp):
        raise InputError('2011 already has entrance %d (%s): pick a free number (free-numbers)' % (number, stock_2011(gp)[number]))
    rep = dict(ok=False, game=g, entrance=entrance, number=number, name=opt(a, '--name', ''), files=[],
               warnings=[], dropped=[], notes=[])
    if lone:
        rep['lone'] = lone
        rep['notes'] += ['lone file %s: %s' % (lone['file'], x) for x in lone['limits']]
        if 'warning' in lone:
            rep['warnings'].append(lone['warning'])
    del PROPS[:]
    try:
        progress(5, 'reading the source')
        conv = CONVERTERS[g]
        kw = {'game_pac': gp} if 'game_pac' in conv.__code__.co_varnames else {}
        evp, evt = conv(src, entrance, number, opt(a, '--name'), '--no-parts' not in a, rep, **kw)
        progress(60, 'checking')
        E08.parse_evp(evp, '2011')
        b = ymks.read_bank(evt)
        for e in b.entries:
            if e['data']:
                for f, r in ymks.parse_stream(e['data']).records():
                    ev, _ = ymks.split_record(r)
                    if ev and not ymks.parse_events(ev, b.T)[1]:
                        raise RuntimeError('event block does not walk 2011\'s size table in (%d,%d,%d)' % (e['id'], e['x'], e['y']))
        files = [(number, evp, evt)]
        if '--no-parts' not in a:
            progress(75, 'Create-An-Entrance parts')
            try:
                files += list(E08.cae_parts(evp, evt, number))
            except Exception as x:   # (scripts without the usual 21 slots: tag / special entrances)
                rep['warnings'].append('no Create-An-Entrance parts for this entrance (%s)' % x)
        progress(90, 'writing the pack')
        os.makedirs(out, exist_ok=True)
        label = '%s entrance %d as 2011 %d' % (GAMES[g]['name'], entrance, number)
        d = EH.write_pack(out, number, files, label)
        EPR.write(d, PROPS)
        if '--no-pyro' not in a and not lone:
            pyro(src, g, entrance, number, d, rep)
        rep['files'] = sorted(os.path.relpath(os.path.join(r, f), out) for r, _, fs in os.walk(d) for f in fs)
        if '--titantron' in a and not lone:
            titantron(src, a, entrance, out, rep)
        rep['out'] = d                 # (the move pack folder; the Mod Maker opens it on the Moves page)
        rep['opens'] = 'moves'
        rep['ok'] = True
    except InputError:
        raise
    except Exception as x:
        rep['warnings'].append('conversion failed: %s' % x)
        os.makedirs(out, exist_ok=True)
        json.dump(rep, open(os.path.join(out, 'report.json'), 'w'), indent=1)
        print(json.dumps(rep))
        sys.exit(3)
    json.dump(rep, open(os.path.join(out, 'report.json'), 'w'), indent=1)
    progress(100, 'done')
    return rep


def convert_victory(a, src, g, lone):
    """--kind victory: one win scene -> a pack with its script and tracks at 2011 number 6000-7999"""
    try:
        scene, number = int(opt(a, '--entrance')), int(opt(a, '--number'))
    except (TypeError, ValueError):
        raise InputError('--entrance and --number must be numbers')
    out = opt(a, '--out')
    if not out:
        raise InputError('--out is needed')
    if not 6000 <= number < 8000:
        raise InputError('--number must be 6000..7999 for a victory scene (free-numbers --kind victory)')
    gp = find_game_pac(opt(a, '--game-pac'))
    if gp and number in stock_victories(gp):
        raise InputError('2011 already has victory scene %d (%s): pick a free number' % (number, stock_victories(gp)[number]))
    rep = dict(ok=False, game=g, entrance=scene, number=number, kind='victory', name=opt(a, '--name', ''), files=[],
               warnings=[], dropped=[], notes=[])
    if lone:
        rep['lone'] = lone
    del PROPS[:]
    try:
        progress(5, 'reading the source')
        conv = CONVERTERS[g]
        kw = {'game_pac': gp} if 'game_pac' in conv.__code__.co_varnames else {}
        evp, evt = conv(src, scene, number, opt(a, '--name'), False, rep, **kw)
        progress(60, 'checking')
        # (2011's own victory scenes give every character object the generic model 13902: the game puts the winner,
        # the loser and the referee in - an entrance's converted model ids would name other superstars)
        e = E08.parse_evp(evp, '2011')
        n_char = 0
        for o in e['objs']:
            if (o['t0'], o['t1']) == E08.TYPE_CHAR and o['model'] != 13902:
                o['model'], n_char = 13902, n_char + 1
        if n_char:
            evp = E08.build_evp(e)
            rep['notes'].append('%d character objects -> the generic model 13902 (as 2011 victory scenes)' % n_char)
        E08.parse_evp(evp, '2011')
        b = ymks.read_bank(evt)
        for e in b.entries:
            if e['data']:
                for f, r in ymks.parse_stream(e['data']).records():
                    ev, _ = ymks.split_record(r)
                    if ev and not ymks.parse_events(ev, b.T)[1]:
                        raise RuntimeError('event block does not walk 2011\'s size table in (%d,%d,%d)' % (e['id'], e['x'], e['y']))
        progress(90, 'writing the pack')
        os.makedirs(out, exist_ok=True)
        d = write_victory_pack(out, number, evp, evt, '%s victory scene %d as 2011 %d' % (GAMES[g]['name'], scene, number))
        EPR.write(d, PROPS)
        rep['files'] = sorted(os.path.relpath(os.path.join(r, f), out) for r, _, fs in os.walk(d) for f in fs)
        rep['out'] = d
        rep['notes'].append('a superstar mod plays it after a singles win with victory=%d (its moves\\ pack or '
                            'Mods\\Moves)' % number)
        rep['ok'] = True
    except InputError:
        raise
    except Exception as x:
        rep['warnings'].append('conversion failed: %s' % x)
        os.makedirs(out, exist_ok=True)
        json.dump(rep, open(os.path.join(out, 'report.json'), 'w'), indent=1)
        print(json.dumps(rep))
        sys.exit(3)
    json.dump(rep, open(os.path.join(out, 'report.json'), 'w'), indent=1)
    progress(100, 'done')
    return rep


def main(argv):
    if not argv or argv[0] in ('-h', '--help'):
        print(__doc__, file=sys.stderr)
        return 2
    global SVRMOD
    SVRMOD = opt(argv, '--svrmod')
    if opt(argv, '--dlc'):   # (the source game's DLC packages: open_source reads them beside the disc)
        os.environ['SVR2011_PORT_DLC'] = opt(argv, '--dlc')
    cmd = {'games': cmd_games, 'detect': cmd_detect, 'list': cmd_list, 'convert': cmd_convert,
           'free-numbers': cmd_free}.get(argv[0])
    if not cmd:
        print('unknown command %r' % argv[0], file=sys.stderr)
        return 2
    try:
        res = cmd(argv[1:])
    except InputError as x:
        print(json.dumps(dict(ok=False, error=str(x))))
        log('error: %s' % x)
        return 2
    print(json.dumps(res))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
