"""Arenas from other games -> SvR 2011 arena mods (docs/MOD_MAKER_3_PLAN.md section 4).

    py tools/arena_port.py games
    py tools/arena_port.py list --game <id> --src <ISO or extract folder>
    py tools/arena_port.py convert --game <id> --src <...> --item <bgNN> --out <dir> [--base <0-19>] [--name <text>]
    py tools/arena_port.py detect --src <one arena .pac>     (which game / arena, what a lone file lacks)
--src may also be one arena pac (list / convert: --game and --item then optional; see 'a lone arena file').

stdout is JSON only; logs and `progress <0-100> <text>` lines go to stderr.
Exit codes: 0 ok, 2 bad input, 3 conversion failed.

What each game needs (docs/OTHER_GAMES.md, "Arenas"):
- svr2010: the arena files are the same EPAC / JBOY / texture sets as 2011's;
  a 2010 arena plays in 2011 unchanged (tested: RAW bg01, 2026-10-13). The
  mod is the file as it is, plus a banner (2010's arena select banner,
  MatchHD MENU/MASI) and, where the base arena's VS theme has one 1024 x 512
  background, a VS and a loading picture from 2010's own VS theme (M<nn>I).
  The game gives the file its slot's stage name (arena_mods.cpp RedirectArena).
- wwe13: converted by the Mod Maker itself (svrfmt/wwe13.cpp); listed here
  as "native".
- svr2008: the same JBOY models, but the arena's texture sets are entries
  0xC8 / 0xC9, ids 2011 uses for models (the 2011 arena load crashed on them):
  they move to 0x190 / 0x191. 2008 has no crowd, announce table pictures,
  card or visibility tree in the arena file: the crowd (0x4E20-0x4E23,
  0x4E2A), 0x192, 0xEE48, 0xEA6C and the turnbuckle covers come from 2011 RAW
  (--pac11 <2011 game folder>). The arena keeps its own lights (0x7530), plus the
  light sets the game looks up itself that it lacks (2007: 11-14). Textures are halved (top mip dropped, biggest
  first) until the file is no bigger than that host's. Pictures: 2008's VS
  theme (M<nn>I) has its art; there is no select banner, so it is cut from it.
  Tested: RAW (bg01) plays in 2011 with the 2011 crowd.
- the rest: research (status "research"; convert exits 3).
"""
import argparse
import io
import json
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import discfs  # noqa: E402
import svrfmt  # noqa: E402

GAMES = [
    {'id': 'svr2007', 'name': "WWE SmackDown vs. Raw 2007", 'platform': 'x360', 'disc_hint': '545107E0', 'status': 'ready'},
    {'id': 'svr2008', 'name': "WWE SmackDown vs. Raw 2008", 'platform': 'x360', 'disc_hint': '5451080B', 'status': 'ready'},
    {'id': 'svr2009', 'name': "WWE SmackDown vs. Raw 2009", 'platform': 'x360', 'disc_hint': '54510826', 'status': 'ready'},
    {'id': 'low', 'name': "WWE Legends of WrestleMania", 'platform': 'x360', 'disc_hint': '54510838', 'status': 'ready'},
    {'id': 'svr2010', 'name': "WWE SmackDown vs. Raw 2010", 'platform': 'x360', 'disc_hint': '54510844', 'status': 'ready'},
    {'id': 'wwe12', 'name': "WWE '12", 'platform': 'x360', 'disc_hint': '54510890', 'status': 'native'},
    {'id': 'wwe13', 'name': "WWE '13", 'platform': 'x360', 'disc_hint': '545108B4', 'status': 'native'},
    {'id': 'wwe2k14', 'name': "WWE 2K14", 'platform': 'x360', 'disc_hint': '545408B2', 'status': 'native'},
    # 2K15: plain EPAC + "ZLIB" entries as 2K14; 2K16: the same inside xcompress; 2K17: xcompressed 'HSPC'
    # with an 'SHDC' stage index (svrfmt/wwe13.cpp HspcStage13) - the same entries and model format
    {'id': 'wwe2k15', 'name': "WWE 2K15", 'platform': 'x360', 'disc_hint': '545408B6', 'status': 'native'},
    {'id': 'wwe2k16', 'name': "WWE 2K16", 'platform': 'x360', 'disc_hint': '315A07D2', 'status': 'native'},
    {'id': 'wwe2k17', 'name': "WWE 2K17", 'platform': 'x360', 'disc_hint': '545408BA', 'status': 'native'},
]
NATIVE_GAMES = ('wwe12', 'wwe13', 'wwe2k14', 'wwe2k15', 'wwe2k16', 'wwe2k17')

# Arenas that convert but crash 2011 at load (full pass, docs/ARENA_PORT_RESULTS.md): 'list' marks
# them status 'crashes' with the reason, so the Mod Maker can grey them out
# (not listed: WWE 2K14 bg44-54 crashed once in the crowd renderer (lr 8217FF84) but played 5 of 5
# repeat runs - bg51 x3, bg44 x2 - and bg55 / 56 likewise: an intermittent crash, open)
KNOWN_CRASHES = {
    # (only arenas that crashed on every repeat run)
    ('wwe13', 'bg37'): 'crashes at load (a memory allocation fails; not its size or models) - 3 of 3 runs, open',
    ('wwe2k14', 'bg00'): 'crashes at load - 2 of 2 runs, open',
    ('wwe2k14', 'bg12'): 'crashes at load (object motion loader, lr 826AEF18) - 3 of 3 runs, open',
    ('wwe2k14', 'bg17'): 'crashes at load (lr 82188A00) - 3 of 3 runs, open',
    # (WWE 2K16 bg06 / bg12 / bg15, 78,935-91,476 vertices against the stock 65,450, crashed at load until 2.1.6:
    # the stage's heap, now given fallback memory - they convert with needs=2.1.6, svrfmt/arena.h)
}


def mark_items(game, items):
    """'list' items get status: 'ok', 'crashes' (KNOWN_CRASHES) or 'special' (WWE '12+ bg60 and up:
    backstage rooms, studio / photo stages - no arenas), and a reason for the last two"""
    for it in items:
        key = it.get('key', '')
        if (game, key) in KNOWN_CRASHES:
            it['status'], it['reason'] = 'crashes', KNOWN_CRASHES[(game, key)]
        elif game in NATIVE_GAMES and not it.get('dlc') and key[:2] == 'bg' and key[2:4].isdigit() and int(key[2:4]) >= 60:
            it['status'], it['reason'] = 'special', 'a backstage room or studio / photo stage, not an arena'
        else:
            it.setdefault('status', 'ok')
    return items

# 2011's arena select tiles: arena number -> banner / manifest base name (arena_mods.cpp kTiles)
BASE_NAMES = {0: 'arena_SD', 1: 'arena_RAW', 2: 'arena_ECW', 3: 'arena_ttt', 4: 'arena_slam', 5: 'arena_BP',
              6: 'arena_HinC', 7: 'arena_Series', 8: 'arena_TLC', 9: 'arena_RR', 10: 'arena_EC', 11: 'arena_WM26',
              12: 'arena_back', 13: 'arena_Judg', 14: 'arena_NoC', 15: 'arena_BASH', 16: 'arena_EXT', 17: 'arena_SS',
              18: 'arena_BR', 19: 'arena_drui'}
# a base theme's one 1024 x 512 VS background (MatchHD MENU/M<nn>I), where it has one
VS_BACKGROUND = {0: 'bg_sd', 1: 'RAW01', 3: 'troops_tex05', 4: 'BG', 11: 'wm_bg01'}

# SvR 2010: arena number -> (select banner name in MASI, show, 2011 arena of that show)
# (from each bgNN's own texture names; bg16 / bg20 / bg22 / bg25 are story copies)
SVR2010 = {
    0: ('arena_SD', 'SmackDown', 0), 1: ('arena_raw', 'RAW', 1), 2: ('arena_ecw', 'ECW', 2),
    4: ('arena_SS', 'SummerSlam', 4), 5: ('arena_UG', 'Unforgiven', None), 6: ('arena_no', 'No Mercy', None),
    7: ('arena_sur', 'Survivor Series', 7), 8: ('arena_arm', 'Armageddon', None), 9: ('arena_RR', 'Royal Rumble', 9),
    10: ('arena_out', 'No Way Out', None), 11: ('arena_WM25', 'WrestleMania 25', None), 12: ('arena_back', 'Backlash', None),
    13: ('arena_judg', 'Judgment Day', 13), 14: ('arena_NoC', 'Night of Champions', 14),
    15: ('arena_GAB', 'The Great American Bash', 15), 18: ('arena_CS', 'Cyber Sunday', None),
    24: (None, 'Extreme Rules', 16),
}
# SvR 2008: arena number -> (show, 2011 arena of that show); bg20-27 are story copies
SVR2008 = {
    0: ('SmackDown', 0), 1: ('RAW', 1), 2: ('ECW', 2), 3: ('Sunday Night Heat', None), 4: ('SummerSlam', 4),
    5: ('Unforgiven', None), 6: ('No Mercy', None), 7: ('Survivor Series', 7), 8: ('Armageddon', None),
    9: ('Royal Rumble', 9), 10: ('No Way Out', None), 11: ('WrestleMania 23', None), 12: ('Backlash', None),
    13: ('Judgment Day', 13), 14: ('Vengeance', None), 15: ('The Great American Bash', 15),
    16: ("New Year's Revolution", None), 17: ('One Night Stand', None), 18: ('December to Dismember', None),
    19: ('Cyber Sunday', None),
}
# SvR 2009: arena number -> (show, 2011 arena of that show); the same layout as 2008's
# (its own crowd and 0x7922 are an older format: replaced by the host's / left out)
SVR2009 = {
    0: ('SmackDown', 0), 1: ('RAW', 1), 2: ('ECW', 2), 3: ('Tribute to the Troops', 3), 4: ('SummerSlam', 4),
    5: ('Unforgiven', None), 6: ('No Mercy', None), 7: ('Survivor Series', 7), 8: ('Armageddon', None),
    9: ('Royal Rumble', 9), 10: ('No Way Out', None), 11: ('WrestleMania XXIV', None), 12: ('Backlash', None),
    13: ('Judgment Day', 13), 14: ('Night of Champions', 14), 15: ('The Great American Bash', 15),
    16: ('One Night Stand', None), 17: ('Cyber Sunday', None), 18: ('Vengeance', None),
}
# SvR 2008 / 2009: arena number -> its picture on the arena select screen (ScrLayHD.pac SCRL/LSSE arena_<key>,
# 256 x 256, the arena itself; Legends of WrestleMania's LSSE is 2009's, none of its own arenas)
SELECT_PICTURE = {
    'svr2008': {0: 'smack', 1: 'raw', 2: 'ecw', 3: 'heat', 4: 'summer', 5: 'unfor', 6: 'nomercy', 7: 'survive',
                8: 'armage', 9: 'rumble', 10: 'noway', 11: 'WM23', 12: 'backlash', 13: 'judge', 14: 'ven', 15: 'GAB',
                16: 'newyear', 17: 'ONS', 18: 'DtoD', 19: 'cybers'},
    'svr2009': {0: 'smack', 1: 'raw', 2: 'ecw', 3: 'troop', 4: 'summer', 5: 'unfor', 6: 'nomercy', 7: 'survive',
                8: 'armage', 9: 'rumble', 10: 'noway', 11: 'WM24', 12: 'backlash', 13: 'judge', 15: 'GAB', 16: 'ONS',
                17: 'cybers', 18: 'ven'},
}
# Legends of WrestleMania (SvR 2009's arena format): arena number -> (show, 2011 arena of that show) - names from its
# string.pac SDB 0100 (674 + n - 200: WrestleMania I - XV), bg216 = MatchHD M D8 I's logoRR08, bg219 its Rumble-match
# copy (the same textures); bg223 / 224 / 225 / 250 are its Create Moves / prop check / approval stages (not listed).
# None of them is in 2011: LOW_BASE is the slot each plays as.
LOW = {**{200 + k: ('WrestleMania %s' % r, None) for k, r in enumerate(
    'I 2 III IV V VI VII VIII IX X XI XII 13 XIV XV'.split(), 1)},
    **{216: ('Royal Rumble 2008', None), 219: ('Royal Rumble 2008 (Rumble match)', None)}}
LOW_BASE = {**{n: 11 for n in range(201, 216)}, 216: 9, 219: 9}
# SvR 2007: stage number -> (show, 2011 arena of that show). Its arenas are
# entries STG<k>/00NN of pac/bg/bg<k>.pac (k = NN // 10); 0020+ are copies.
SVR2007 = {
    0: ('SmackDown', 0), 1: ('RAW', 1), 2: ('ECW', 2), 4: ('SummerSlam', 4), 5: ('Unforgiven', None),
    6: ('No Mercy', None), 7: ('Survivor Series', 7), 8: ('Armageddon', None), 9: ('Royal Rumble', 9),
    10: ('No Way Out', None), 11: ('WrestleMania 22', None), 12: ('Backlash', None), 13: ('Judgment Day', 13),
    14: ('Vengeance', None), 15: ('The Great American Bash', 15), 16: ("New Year's Revolution", None),
    17: ('One Night Stand', None), 18: ('Cyber Sunday', None), 19: ("Saturday Night's Main Event", None),
}
# (0x7922: the arena's motion bank - crowd, ring crew; 2011's adds motion 500, the ring announcer's intro, which
# SvR 2007-2010 lack: without it he stands in the bind pose in the intro)
HOST_ENTRIES = (0x192, 0x4E20, 0x4E21, 0x4E22, 0x4E23, 0x4E2A, 0x7922, 0xEE48, 0xEA6C)
MOTION_BANK = 0x7922
# SvR 2007-2009 turnbuckle covers (ar_cover10-13): a black logo pad (rin_tb00) inside a white taped
# shell (rin_tb10) that 2011 draws over the pad - the "white turnbuckles". 2011 RAW's covers are the
# same black logo pads with no shell, on the same ring.
HOST_COVERS = (0x3C4, 0x3C5, 0x3C6, 0x3C7)
MAX_ARENA = 9_400_000  # bytes: the smaller 2011 hosts (until 2.1.6 a bigger file than its slot's crashed)
# Since 2.1.6 an arena may be bigger than its slot's shipped file (the game reads its stage at its own size:
# src/arena_mods.cpp) - up to ARENA_FILE_MAX (tested 21.5 MB: 1v1 and a 6-man at 60 fps, 27 Oct) - and hold more 3D
# detail than 2011's own (STOCK_VERTS; src/ring_rules.cpp): such an arena says needs=model_budget.NEEDS_VERSION, older
# games crash loading it.
ARENA_FILE_MAX = 21 << 20
STOCK_VERTS = 65450
# Meshes (the stage's drawn pieces): the game's own arenas have up to STOCK_MESHES (bg01). Each is drawn with its own
# render commands every frame; past MAX_MESHES the match's command queue runs full in a 6-man and the game crashes
# (27 Oct: 1,770 meshes crashed with stock wrestlers; 1,062 held 5 minutes with six 1,489-page superstars) - the same
# numbers as the Mod Maker's (modmaker/svrfmt/arena.h kMaxArenaMeshes / kMaxArenaMeshesNew).
STOCK_MESHES = 446
MAX_MESHES = 900


def stage_vertices(data, meshes_out=None):
    """every JBOY model's vertices in an arena pac's STG entries (and their meshes into meshes_out[0])"""
    import jboy
    n = 0
    for typ, ents in svrfmt.epac_read(data)[1]:
        if typ != b'STG ':
            continue
        for _, blob in ents:
            if blob[:4] != b'PACH':
                continue
            for _, cb in svrfmt.pach_read(blob):
                raw = svrfmt.bpe_decode(cb) if cb[:4] == b'BPE ' else cb
                if raw[:4] == b'JBOY':
                    try:
                        m = jboy.read(raw)
                        n += sum(len(s.verts) for s in m.meshes)
                        if meshes_out is not None:
                            meshes_out[0] += len(m.meshes)
                    except Exception:  # noqa: BLE001
                        pass
    return n


def crowd_area_line(data):
    """the manifest's crowd_area= line from an SvR 2008 arena's own crowd fighting areas ('' when it has none)"""
    import crowd_walkway
    try:
        area = crowd_walkway.crowd_area(data)
    except Exception:  # (an arena without the stage text)
        return ''
    return 'crowd_area=%s\n' % area if area else ''


def needs_line(data, host_size):
    """the manifest's needs= line for an arena bigger than its slot's file or with more 3D detail ('' when neither);
    BadInput for more meshes than the game can draw (MAX_MESHES)"""
    import model_budget
    meshes = [0]
    verts = stage_vertices(data, meshes)
    if meshes[0] > MAX_MESHES:
        raise BadInput('the arena has %d separate meshes; SvR 2011 can draw up to %d in a match (more crash a 6-man): '
                       'leave some objects out or join small ones' % (meshes[0], MAX_MESHES))
    if len(data) > host_size or verts > STOCK_VERTS or meshes[0] > STOCK_MESHES:
        return 'needs=%s\n' % model_budget.NEEDS_VERSION
    return ''


class BadInput(Exception):
    pass


def log(*a):
    print(*a, file=sys.stderr, flush=True)


def progress(p, text):
    print('progress %d %s' % (p, text), file=sys.stderr, flush=True)


def bpe(raw):
    """a BPE entry: svrmod.exe's encoder (seconds for an arena's textures), else the pure Python one
    (minutes: 2.5 of a SvR 2008 arena's 2.6 went there), checked by unpacking it again"""
    try:
        out = svrfmt.bpe_pack(raw)
        if svrfmt.bpe_decode(out) == raw:
            return out
        log('svrmod bpe: the packed entry does not unpack the same - the Python encoder instead')
    except Exception as e:  # noqa: BLE001 - no svrmod.exe / it failed: the slow way
        log('svrmod bpe unavailable (%s): the Python encoder (slow)' % e)
    return svrfmt.bpe_encode(raw, compress=True)


def textures(raw):
    """A texture set (u32 count, 32-byte records name[16] 'dds' size offset) -> [(name, dds bytes)]."""
    n = struct.unpack_from('<I', raw, 0)[0]
    if not (0 < n < 600) or 16 + 32 * n > len(raw) or raw[32:35] != b'dds':
        return []
    out = []
    for i in range(n):
        r = raw[16 + 32 * i:48 + 32 * i]
        size, off = struct.unpack_from('<II', r, 20)
        out.append((r[:16].split(b'\0')[0].decode('latin-1'), raw[off:off + size]))
    return out


def group_textures(pac_bytes, entry_name):
    """Every texture of an EPAC entry (MENU/<entry_name>), from its PACH children."""
    _, groups, _ = svrfmt.epac_read(pac_bytes)
    out = []
    for _, ents in groups:
        for nm, blob in ents:
            if nm.split(b'\0')[0].decode('latin-1') != entry_name or blob[:4] != b'PACH':
                continue
            for _, child in svrfmt.pach_read(blob):
                raw = svrfmt.bpe_decode(child) if child[:4] == b'BPE ' else child
                out += textures(raw)
    return out


def image(dds):
    from PIL import Image
    return Image.open(io.BytesIO(dds)).convert('RGBA')


def dds_dxt5(img):
    b = io.BytesIO()
    img.convert('RGBA').save(b, 'DDS', pixel_format='DXT5')
    return b.getvalue()


def fit(img, w, h):
    """img scaled to cover w x h, centred and cropped."""
    from PIL import Image
    s = max(w / img.width, h / img.height)
    im = img.resize((max(1, round(img.width * s)), max(1, round(img.height * s))), Image.LANCZOS)
    x, y = (im.width - w) // 2, (im.height - h) // 2
    return im.crop((x, y, x + w, y + h))


def banner_tile(banner, art):
    """A 256 x 128 tile: a wide banner fills it; a square one (2010's PPVs) is
    shown whole, over its VS picture darkened (or black)."""
    from PIL import Image, ImageEnhance
    if banner.width >= 1.6 * banner.height:
        return fit(banner, 256, 128)
    bg = fit(art, 256, 128) if art is not None else Image.new('RGBA', (256, 128), (0, 0, 0, 255))
    bg = ImageEnhance.Brightness(bg.convert('RGB')).enhance(0.45).convert('RGBA')
    s = 128 / banner.height
    b = banner.resize((max(1, round(banner.width * s)), 128), Image.LANCZOS)
    bg.paste(b, ((256 - b.width) // 2, 0), b)
    return bg


# -- SvR 2010 ------------------------------------------------------------------

def svr2010_list(disc):
    items = []
    for n, (banner, show, in11) in sorted(SVR2010.items()):
        path = 'pac/bg/bg%02d.pac' % n
        if not disc.exists(path):
            continue
        items.append({'key': 'bg%02d' % n, 'name': show, 'arena': n, 'size': disc.size(path),
                      'banner': banner, 'in_2011': in11})
    return items


def svr2010_convert(disc, key, out, base=None, name=None, pac11=None, year='2010'):
    warnings, notes, dropped = [], [], []
    try:
        n = int(key[2:]) if key.lower().startswith('bg') else int(key)
    except ValueError:
        raise BadInput('item %r: expected bgNN' % key)
    lone = getattr(disc, 'lone', False)
    if n not in SVR2010 and not lone:
        raise BadInput('item %r: not an arena of SvR 2010 (story copies are left out)' % key)
    banner_name, show, in11 = SVR2010.get(n) if year == '2010' and n in SVR2010 else (None, 'Arena bg%02d' % n, None)
    path = 'pac/bg/bg%02d.pac' % n
    if not disc.exists(path):
        raise BadInput('%s not on the disc' % path)
    if base is None:
        base = in11 if in11 is not None else (n if n in BASE_NAMES else 3)
    if base not in BASE_NAMES:
        raise BadInput('base %r: 0-19' % base)
    name = name or '%s (SvR %s)' % (show, year)
    mod_id = 'svr%s_' % year + ''.join(c if c.isalnum() else '_' for c in show.lower()).strip('_')

    progress(5, 'reading %s' % path)
    arena = disc.read(path)
    if arena[:4] != b'EPAC':
        raise RuntimeError('%s: not an EPAC arena' % path)
    host_size = os.path.getsize(find_host(pac11, base)) if pac11 else MAX_ARENA
    if len(arena) > host_size:
        notes.append('the arena file is %d bytes, bigger than its slot\'s (%d): it needs version 2.1.6 or newer'
                     % (len(arena), host_size))
    mod = os.path.join(out, mod_id)
    os.makedirs(os.path.join(mod, 'vs'), exist_ok=True)
    # (its motion bank lacks 2011's motion 500, the ring announcer's intro: 2011 RAW's in its place)
    if pac11:
        try:
            _, graw, _ = svrfmt.epac_read(open(find_host(pac11, 1), 'rb').read())
            bank = dict(svrfmt.pach_read(graw[0][1][0][1])).get(MOTION_BANK)
            ha, ga, ta = svrfmt.epac_read(arena)
            ents = dict(svrfmt.pach_read(ga[0][1][0][1]))
            if bank is not None and MOTION_BANK in ents:
                ents[MOTION_BANK] = bank
                g = [(ga[0][0], [(ga[0][1][0][0], svrfmt.pach_write(sorted(ents.items())))])] + list(ga[1:])
                arena = svrfmt.epac_write(ha, g, ta)
                notes.append("motion bank (crowd, ring crew, the announcer's intro) from 2011 RAW")
        except Exception as e:  # (the arena as it is - logged)
            warnings.append('motion bank not replaced: %s' % e)
    with open(os.path.join(mod, 'arena.pac'), 'wb') as f:
        f.write(arena)
    # (SvR 2010's match light sets are far darker than 2011's in some arenas - WrestleMania 25: set 10 ground 0.20 against
    # RAW's 0.78 - wrestlers and ring dark in the match: those sets from 2011 RAW, as the Mod Maker's "Repair it" and
    # its WWE '12+ converter do; svrmod lights, which leaves an arena that isn't dark as it is)
    if pac11:
        try:
            import subprocess
            import wwe13_entrance as WE
            pac = os.path.join(mod, 'arena.pac')
            r = subprocess.run([WE.find_svrmod(None), 'lights', pac, find_host(pac11, 1), pac], capture_output=True,
                               text=True, timeout=120)
            dark = [ln.split(':', 1)[1].strip() for ln in r.stdout.splitlines() if ln.startswith('dark sets:')]
            if dark and dark[0]:
                notes.append('match lighting too dark for SvR 2011 - 2011 RAW\'s sets used: %s' % dark[0])
        except Exception as e:  # noqa: BLE001 - no svrmod.exe: the arena's own lights (the doctor offers the repair)
            warnings.append('match lighting not checked: %s' % e)

    progress(40, 'pictures')
    match = disc.read('pac/menu/MatchHD.pac') if disc.exists('pac/menu/MatchHD.pac') else None
    banner = None
    if match and banner_name:
        for nm, dds in group_textures(match, 'MASI'):
            if nm == banner_name:
                banner = image(dds)
    art = None  # the 2010 VS theme's biggest picture
    if match:
        theme = group_textures(match, 'M%02dI' % n)
        best = 0
        for nm, dds in theme:
            try:
                im = image(dds)
            except Exception:
                continue
            if im.width * im.height > best and im.width >= 256:
                art, best = im, im.width * im.height
    if banner is None and art is not None:
        banner = art
        notes.append('no select banner on the disc for this arena: the banner is cut from its VS picture')
    from PIL import Image
    if banner is None:
        banner = host_banner(pac11, base)
        if banner is not None:
            notes.append("no banner or VS picture %s: 2011's %s banner" % ('in a lone arena file' if lone else 'on the disc',
                                                                          BASE_NAMES[base]))
    if banner is None:
        banner = Image.new('RGBA', (256, 128), (30, 30, 30, 255))
        warnings.append('no banner or VS picture found: a plain banner was made')
    with open(os.path.join(mod, 'banner.dds'), 'wb') as f:
        f.write(dds_dxt5(banner_tile(banner, art)))
    if art is not None:
        with open(os.path.join(mod, 'load.dds'), 'wb') as f:
            f.write(dds_dxt5(fit(art, 1024, 512)))
        if base in VS_BACKGROUND:
            with open(os.path.join(mod, 'vs', VS_BACKGROUND[base] + '.dds'), 'wb') as f:
                f.write(dds_dxt5(fit(art, 1024, 512)))
        else:
            notes.append("the VS screen keeps %s's theme (it has no single 1024 x 512 background)" % BASE_NAMES[base])
    else:
        notes.append('no loading or VS picture (no VS theme for this arena on the disc)')
    # its own VS screen: the disc's whole theme for this arena (vs_port.py; the game shows it in this arena's matches
    # - games before vs/theme.pac keep the picture above)
    vs_theme = None
    if year == '2010' and not lone and pac11 and match:
        try:
            import vs_port
            vs_port.cmd_theme('svr2010', disc.path, pac11, n, os.path.join(mod, 'vs', 'theme.pac'))
            vs_theme = 'SvR 2010 %s' % show
            notes[:] = [x for x in notes if not x.startswith('the VS screen keeps')]
            notes.append("its own VS screen: SvR 2010's %s theme (vs/theme.pac)" % show)
        except Exception as e:  # (the picture only - logged)
            warnings.append('no own VS screen: %s' % e)
    if not os.listdir(os.path.join(mod, 'vs')):
        os.rmdir(os.path.join(mod, 'vs'))

    with open(os.path.join(mod, 'manifest.txt'), 'w', newline='\n') as f:
        f.write('type=arena\nid=%s\nname=%s\nauthor=SvR %s arena %s, ported by port tools\nversion=1.0\nbase=%s\n'
                % (mod_id, name, year, 'bg%02d' % n, BASE_NAMES[base]) + needs_line(arena, host_size)
                + ('vs_theme=%s\n' % vs_theme if vs_theme else '') + crowd_area_line(arena))
    progress(80, 'packing')
    import wwe13_pack
    svrmod = os.path.join(out, mod_id + '.svrmod')
    old = sys.stdout
    sys.stdout = sys.stderr  # (the packer prints a line)
    try:
        wwe13_pack.pack(mod, svrmod, 'Port tools (SvR %s)' % year)
    finally:
        sys.stdout = old
    notes.append('base %s (arena %d: it plays as that arena, wherever its tile is on the custom arena pages); the game renames its stage to the slot\'s' % (BASE_NAMES[base], base))
    if n not in (0, 1, 2, 4, 7, 9, 13, 14, 15) and in11 is None:
        notes.append('2011 has no %s arena: the mod plays as the %s arena' % (show, BASE_NAMES[base]))
    progress(100, 'done')
    return {'ok': True, 'out': svrmod, 'folder': mod, 'opens': 'arena', 'warnings': warnings, 'dropped': dropped,
            'notes': notes}


def drop_top_mip(dds):
    """A DDS one size down: its top level removed (the next mip becomes the top)."""
    h, w = struct.unpack_from('<II', dds, 12)
    mips = struct.unpack_from('<I', dds, 28)[0]
    four = dds[84:88]
    if mips < 2 or four not in (b'DXT1', b'DXT3', b'DXT5') or w < 8 or h < 8:
        return None
    block = 8 if four == b'DXT1' else 16
    top = max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * block
    out = bytearray(dds[:128])
    struct.pack_into('<II', out, 12, h // 2, w // 2)
    struct.pack_into('<I', out, 28, mips - 1)
    if struct.unpack_from('<I', out, 20)[0]:  # pitch / linear size
        struct.pack_into('<I', out, 20, top // 4)
    return bytes(out) + dds[128 + top:]


def texture_set_write(texs, head):
    n = len(texs)
    off = (16 + 32 * n + 15) & ~15
    recs, blobs = b'', b''
    for name, dds in texs:
        recs += (name.encode('latin-1')[:15].ljust(16, b'\0') + b'dds\0'
                 + struct.pack('<II', len(dds), off + len(blobs)) + bytes(4))
        blobs += dds + bytes(-len(dds) % 16)
    return (struct.pack('<I', n) + head[4:16] + recs).ljust(off, b'\0') + blobs


def find_host(pac11, base):
    for p in (os.path.join(pac11, 'pac', 'bg', 'bg%02d.pac' % base), os.path.join(pac11, 'bg%02d.pac' % base),
              os.path.join(pac11, 'bg', 'bg%02d.pac' % base)):
        if os.path.exists(p):
            return p
    raise BadInput('--pac11 %s: no bg%02d.pac (give the SvR 2011 game folder)' % (pac11, base))


# Arena lights: entry 0x7530 of each arena is a YMKs bank of light sets (ids 10-14, 20, 30, 100-117,
# 200-202, 300-306, 450-454, 500-503 in 2011 RAW). Each set: ground / sky colour (y 0 / 1), up to 4
# directional lights (y 10-13) and moving lights (y 50-59), as 25-byte keys (position, rotation,
# colour). The model flags (0xC351) pick a set per model with l(n), the light zones (0xC353) per
# area of the floor (entrance stage, ramp); a model without l() gets set 100. Every SvR 2007-2010
# and WWE '12-2K14 arena has its own bank in this format, with the same id plan.
EXE_LIGHTS = (10, 11, 12, 13, 14, 20, 30, 31, 100, 110, 115, 500)  # sets the game looks up itself


def light_bank(raw):
    """0x7530 bytes (2007-2009: no YMKs header, which 2011 accepts) -> ymks.Bank"""
    import ymks
    if raw[:4] not in (b'YMKs', b'YMBs'):
        raw = b'YMKs' + struct.pack('<I', 0x100) + bytes(8) + raw
    return ymks.read_bank(raw)


def light_ids(bank):
    return sorted({e['id'] for e in bank.entries})


def fill_lights(bank, host_bank, ids=EXE_LIGHTS, replace=()):
    """Light sets the game looks up by number that the arena's bank lacks (SvR 2007 has no 11-14;
    its own bank crashed the 2011 load without them) are copied from 2011 RAW's -> [ids added].
    Sets in `replace` are RAW's even when the arena has them."""
    if replace:
        bank.entries = [e for e in bank.entries if e['id'] not in replace]
    have = set(light_ids(bank))
    added = []
    for i in list(ids) + [r for r in replace if r not in ids]:
        if i in have:
            continue
        ents = [dict(e) for e in host_bank.entries if e['id'] == i]
        if ents:
            ents[0]['share'] = False
            bank.entries += ents
            added.append(i)
    return added


def remap_lights(flags, have):
    """Model flag text (0xC351): l(n) / le(n) / lg(n) sets missing from the light bank the arena
    ships (have: its ids) go to the nearest set of the same hundred (an unknown set is index -1
    in the game)."""
    import re
    have = sorted(have)
    missing = {}

    def sub(m):
        n = int(m.group(2))
        if n in have or not have:
            return m.group(0)
        same = [h for h in have if h // 100 == n // 100] or have
        to = min(same, key=lambda h: abs(h - n))
        missing[n] = to
        return m.group(1) + b'(%d)' % to
    return re.sub(rb'\b(l[eg]?)\((\d+)\)', sub, flags), missing


def vis_tree(ents):
    """A visibility tree (entry 0xC364) for an arena that has none: SvR2011 culls every arena
    model its tree doesn't list (2007-2009 arenas came out with a black floor and no barricades).
    One node per arena model (ids below 0x190), boxed by its vertices: 3ds Max axes in metres,
    (x, z, -y) x 0.1 of the model's units, as 2011's own trees."""
    import jboy
    boxes = []
    for i, x in sorted(ents.items()):
        if i >= 0x190:
            continue
        d = svrfmt.bpe_decode(x) if x[:4] == b'BPE ' else x
        if d[:4] != b'JBOY':
            continue
        m = jboy.read(d)
        v = [p for msh in m.meshes for p in msh.verts]
        if not v:
            continue
        xs, ys, zs = [p[0] * 0.1 for p in v], [p[2] * 0.1 for p in v], [-p[1] * 0.1 for p in v]
        boxes.append((m.name, (min(xs), min(ys), min(zs), max(xs), max(ys), max(zs))))
    if not boxes:
        return None
    root = [min(b[1][k] for b in boxes) for k in range(3)] + [max(b[1][k] for b in boxes) for k in range(3, 6)]

    def f(b):
        return ' '.join('%g' % round(c, 5) for c in b)
    out = [';<\u30a2\u30ea\u30fc\u30ca\u30e2\u30c7\u30eb\u5206\u5272\u306b\u3088\u308b\u53ef\u8996\u5224\u5b9a'
           '\u30c7\u30fc\u30bf\u30d5\u30a1\u30a4\u30eb>', ';\u5ea7\u6a19\u7cfb=3ds max', '',
           'NODE MY_NAME = VT_ROOT', '{', '\t' + f(root), '}', str(len(boxes)), '']
    for name, b in boxes:
        out += ['NODE MY_NAME = %s PARENT_NAME = VT_ROOT' % name, '{', '\t' + f(b), '}', '0', '']
    return '\r\n'.join(out).encode('cp932')


def svr2007_path(n):
    return 'pac/bg/bg%d.pac' % (n // 10)


def svr2008_list(disc, table=None):
    items = []
    for n, (show, in11) in sorted((table or SVR2008).items()):
        path = svr2007_path(n) if table is SVR2007 else 'pac/bg/bg%02d.pac' % n
        if disc.exists(path):
            items.append({'key': 'bg%02d' % n, 'name': show, 'arena': n, 'size': disc.size(path), 'in_2011': in11})
    return items


def svr2008_convert(disc, key, out, pac11, base=None, name=None, game='svr2008'):
    table = {'svr2009': SVR2009, 'svr2007': SVR2007, 'low': LOW}.get(game, SVR2008)
    year = game[3:]
    label = 'Legends of WrestleMania' if game == 'low' else 'SvR %s' % year
    warnings, notes, dropped = [], [], []
    try:
        n = int(key[2:]) if key.lower().startswith('bg') else int(key)
    except ValueError:
        raise BadInput('item %r: expected bgNN' % key)
    lone = getattr(disc, 'lone', False)
    if n not in table and not lone:
        raise BadInput('item %r: not an arena of %s (story copies are left out)' % (key, label))
    if not pac11:
        raise BadInput('SvR 2007 - 2009 arenas need --pac11 <SvR 2011 game folder> (crowd and table pictures)')
    show, in11 = table.get(n, ('Arena bg%02d' % n, None))
    if base is None:
        base = in11 if in11 is not None else LOW_BASE.get(n) if game == 'low' else (n if n in BASE_NAMES else 3)
        base = 3 if base is None else base
    if base not in BASE_NAMES:
        raise BadInput('base %r: 0-19' % base)
    name = name or ('%s (LoW)' % show if game == 'low' else '%s (SvR %s)' % (show, year))
    mod_id = game + '_' + ''.join(c if c.isalnum() else '_' for c in show.lower()).strip('_')
    progress(3, 'reading bg%02d' % n)
    # the file's layout and size budget: the base slot's arena; the borrowed entries (crowd,
    # table pictures, card, light set): always 2011's RAW (a 2008 arena with SummerSlam's
    # crowd / lights crashed the load on its slot; with RAW's it plays there)
    host = open(find_host(pac11, base), 'rb').read()
    hb, gb, tb = svrfmt.epac_read(host)
    _, graw, _ = svrfmt.epac_read(open(find_host(pac11, 1), 'rb').read())
    eb = dict(svrfmt.pach_read(graw[0][1][0][1]))
    if game == 'svr2007':  # one stage of a many-stage pac
        _, ga, _ = svrfmt.epac_read(disc.read(svr2007_path(n)))
        stage = next((b for typ, es in ga for nm, b in es if typ.startswith(b'STG') and nm == b'%04d' % n), None)
        if stage is None:
            raise BadInput('%s has no stage %04d' % (svr2007_path(n), n))
    else:
        _, ga, _ = svrfmt.epac_read(disc.read('pac/bg/bg%02d.pac' % n))
        stage = ga[0][1][0][1]
    ents = {}
    for i, x in svrfmt.pach_read(stage):
        # (2007: its crowd is loose models and seat files 0x4E20-0x52FF, plus 0x9C40-0x9CFF)
        if game == 'svr2007' and (0x4E20 <= i <= 0x52FF or 0x9C40 <= i <= 0x9CFF):
            continue
        ents[{0xC8: 0x190, 0xC9: 0x191}.get(i, i)] = x
    if 0x190 not in ents:
        raise RuntimeError('bg%02d: no texture set (0xC8)' % n)
    for i in (0x4E20, 0x4E21, 0x4E22, 0x4E23, 0x4E2A, 0x7922):  # older crowd / motion formats
        ents.pop(i, None)
    for i in HOST_ENTRIES + HOST_COVERS:
        if i in eb:
            ents[i] = eb[i]
    unp = lambda b: svrfmt.bpe_decode(b) if b[:4] == b'BPE ' else b  # noqa: E731
    have = None
    progress(8, 'lights')
    if 0x7530 in ents and 0x7530 in eb:  # the arena's own lights, plus the sets the game needs
        import ymks
        bank = light_bank(unp(ents[0x7530]))
        # (set 10, the game's own light for the wrestlers and the ring, is the same in every arena
        # of a game; SvR 2008's side lights run at 1.2-3.0x - the others' at most ~1.5x, 2011's
        # 0.6x - and blew the black turnbuckle pads out to cream in 2011. With SvR 2008's or 2009's
        # set 10 some arenas crashed ~10 s into a match in the code that reads it (sub_822A6BD8,
        # lr 822A6EE8: 2008 bg04 / bg06 / bg07, 2009 bg09 / bg17, run to run): RAW's)
        added = fill_lights(bank, light_bank(unp(eb[0x7530])), replace=(10,) if game in ('svr2008', 'svr2009', 'low') else ())
        if added:
            ents[0x7530] = ymks.write_bank(bank)
            notes.append('light sets the game needs, from 2011 RAW: ' + ', '.join(map(str, added)))
        have = light_ids(bank)
    if 0xC364 not in ents:
        progress(12, 'visibility tree')
        tree = vis_tree(ents)
        if tree:
            ents[0xC364] = bpe(tree)
            notes.append('visibility tree made from the arena models (the game had none)')
    if 0xC351 in ents and have:
        flags, moved = remap_lights(unp(ents[0xC351]), have)
        if moved:
            ents[0xC351] = bpe(flags)
            notes.append('light sets missing from the arena, moved: ' + ', '.join('%d -> %d' % kv for kv in sorted(moved.items())))
    notes.append('crowd, announce table pictures and card from 2011 RAW')

    def build(tex_blob):
        e = dict(ents)
        e[0x190] = tex_blob
        g = [(gb[0][0], [(gb[0][1][0][0], svrfmt.pach_write(sorted(e.items())))])] + list(gb[1:])
        return svrfmt.epac_write(hb, g, tb)

    # (bigger than the host - needs= in the manifest - only when the stage is the pac's last entry: the game reads
    # every entry at the place the host had it, so only the file's end may move)
    budget = max(len(host), ARENA_FILE_MAX) if len(gb) == 1 else len(host)
    progress(16, 'fitting the textures into the %s file' % BASE_NAMES[base])
    data = build(ents[0x190])
    raw = svrfmt.bpe_decode(ents[0x190])
    texs = textures(raw)
    head = raw[:16]
    halved = set()
    rnd = 0
    while len(data) > budget:
        rnd += 1
        if rnd > 6:
            raise RuntimeError('cannot fit bg%02d into %d bytes' % (n, budget))
        over = len(data) - budget
        saved = 0
        for k in sorted(range(len(texs)), key=lambda k: -len(texs[k][1])):
            if saved * 0.67 > over * 1.15:
                break
            smaller = drop_top_mip(texs[k][1])
            if smaller is None:
                continue
            saved += len(texs[k][1]) - len(smaller)
            texs[k] = (texs[k][0], smaller)
            halved.add(texs[k][0])
        progress(10 + rnd * 12, 'packing textures, round %d' % rnd)
        data = build(bpe(texture_set_write(texs, head)))
        log('round %d: file %d, budget %d' % (rnd, len(data), budget))
    if halved:
        notes.append('%d textures at half size to fit the %s file (%d bytes)' % (len(halved), BASE_NAMES[base], budget))

    mod = os.path.join(out, mod_id)
    os.makedirs(os.path.join(mod, 'vs'), exist_ok=True)
    with open(os.path.join(mod, 'arena.pac'), 'wb') as f:
        f.write(data)
    if game == 'svr2008':  # (its crowd area opened for 2011's crowd fighting: a walkway, the climb's area)
        import crowd_walkway
        notes.append('crowd area: %s' % crowd_walkway.patch_pac(os.path.join(mod, 'arena.pac')))
    progress(85, 'pictures')
    art = None
    if disc.exists('pac/menu/MatchHD.pac'):
        best = 0
        for nm, dds in group_textures(disc.read('pac/menu/MatchHD.pac'), ('M%02XI' if game == 'low' else 'M%02dI') % n):
            try:
                im = image(dds)
            except Exception:
                continue
            if im.width >= 256 and im.width * im.height > best:
                art, best = im, im.width * im.height
    from PIL import Image
    # the arena's own picture from its game's arena select (SELECT_PICTURE): the banner, cropped wide
    photo = None
    key = SELECT_PICTURE.get(game, {}).get(n)
    if key and disc.exists('pac/menu/ScrLayHD.pac'):
        try:
            got = dict(group_textures(disc.read('pac/menu/ScrLayHD.pac'), 'LSSE')).get('arena_' + key)
            photo = image(got) if got else None
            if photo is not None:  # (inside its white frame: 8 of 256 each side)
                m = max(1, photo.width * 8 // 256)
                photo = photo.crop((m, m, photo.width - m, photo.height - m))
        except Exception as e:  # noqa: BLE001 - the VS picture's banner instead
            warnings.append('arena select picture arena_%s not read: %s' % (key, e))
    banner = photo if photo is not None else art
    if photo is not None:
        notes.append('banner: the arena select picture arena_%s' % key)
    elif art is not None:
        notes.append('banner: cut from the VS picture (no arena select picture for this arena)')
    if banner is None:
        banner = host_banner(pac11, base)
        if banner is not None:
            notes.append("no VS picture %s: 2011's %s banner" % ('in a lone arena file' if lone else 'on the disc',
                                                               BASE_NAMES[base]))
    if banner is None:
        warnings.append('no VS picture for this arena on the disc: a plain banner was made')
        banner = Image.new('RGBA', (256, 128), (30, 30, 30, 255))
    with open(os.path.join(mod, 'banner.dds'), 'wb') as f:
        f.write(dds_dxt5(fit(banner, 256, 128)))
    if art is not None or photo is not None:
        with open(os.path.join(mod, 'load.dds'), 'wb') as f:
            f.write(dds_dxt5(fit(art if art is not None else photo, 1024, 512)))
        notes.append('loading picture: %s' % ('the VS picture' if art is not None else 'the arena select picture'))
    if art is not None:
        if base in VS_BACKGROUND:
            with open(os.path.join(mod, 'vs', VS_BACKGROUND[base] + '.dds'), 'wb') as f:
                f.write(dds_dxt5(fit(art, 1024, 512)))
    if not os.listdir(os.path.join(mod, 'vs')):
        os.rmdir(os.path.join(mod, 'vs'))
    with open(os.path.join(mod, 'manifest.txt'), 'w', newline='\n') as f:
        f.write('type=arena\nid=%s\nname=%s\nauthor=%s arena bg%02d, ported by port tools\nversion=1.0\nbase=%s\n'
                % (mod_id, name, label, n, BASE_NAMES[base]) + needs_line(data, len(host))
                + (crowd_area_line(data) if game == 'svr2008' else ''))
    progress(92, 'packing the mod')
    import wwe13_pack
    svrmod = os.path.join(out, mod_id + '.svrmod')
    old = sys.stdout
    sys.stdout = sys.stderr
    try:
        wwe13_pack.pack(mod, svrmod, 'Port tools (%s)' % label)
    finally:
        sys.stdout = old
    if in11 is None:
        notes.append('2011 has no %s arena: the mod plays as the %s arena' % (show, BASE_NAMES[base]))
    progress(100, 'done')
    return {'ok': True, 'out': svrmod, 'folder': mod, 'opens': 'arena', 'warnings': warnings, 'dropped': dropped,
            'notes': notes}


def bg_list(disc):
    """Every arena file of the disc's pac/bg (bgNN.pac, WWE '12's bg200 / bg201 too; not bgEtc)."""
    import re
    items = []
    for nm in disc.listdir('pac/bg'):
        m = re.fullmatch(r'bg(\d+)(_dlc\d+)?\.pac', nm.lower())   # (a package's: 2K15 bg51.pac, 2K16 bg60_dlc40.pac)
        if m:
            items.append({'key': nm.lower()[:-4], 'name': nm.lower()[:-4], 'arena': int(m.group(1)),
                          'size': disc.size('pac/bg/' + nm), 'in_2011': None})
            if hasattr(disc, 'dlc') and not disc.disc.exists('pac/bg/' + nm):
                items[-1]['dlc'] = True
    return sorted(items, key=lambda it: it['arena'])


def dlc_files(disc, game, src, items, cache_root=None):
    """a package's arena, copied out once: 'dlc_file' (the Mod Maker's own converter takes a file). 'name' ends " (DLC)"."""
    import arena_names as AN
    folder = os.path.join(AN.cache_dir(os.path.abspath(cache_root or arena_cache_root()), game, src), 'dlc')
    for it in items:
        if not it.get('dlc'):
            continue
        dst = os.path.join(folder, it['key'] + '.pac')
        if not os.path.exists(dst) or os.path.getsize(dst) != it['size']:
            os.makedirs(folder, exist_ok=True)
            tmp = dst + '.part'
            with open(tmp, 'wb') as f:
                f.write(disc.read('pac/bg/' + it['key'] + '.pac'))
            os.replace(tmp, dst)
        it['dlc_file'] = dst
        # (2K15's six are all named RAW; 2K16's have no name in the game's tables)
        named = it['name'] if it['name'] != it['key'] else 'Arena'
        it['name'] = '%s bg%d (DLC)' % (named, it['arena'])
    return items


# -- a lone arena file (no disc) -------------------------------------------------
# list / convert / detect take --src <one arena .pac> (the file as it is on the disc, any name);
# the game is told from the file. The markers, checked on every bgNN.pac (bg00-59) of SvR 2008-2011
# and WWE '12-2K16 (2026-10-15; 2007 / 2K17 by their containers):
#   xcompress (0FF512ED) around an HSPC -> 2K17 (around an EPAC / EPK8: '13 - 2K16, below)
#   EPAC groups STG<k>, ten stages each -> 2007
#   no texture set 0x190 (it is 0xC8) -> 2008 / 2009 (2009: a crowd 0x4E20 with parts, 0x7922, 0xC5 / 0xC6)
#   ZLIB entries and 0x7532 -> 2K14 - 2K16 (EPK8 or 0x3E6: 2K15; 0x179-0x181 / 0x334 / 0x3CF-0x3D8 or a
#     72-part crowd: 2K16; 0x3E7: 2K14, else 2K16 - 'sure' false where they can't be told apart: the same convert)
#   BPE entries and 0x7532 -> WWE '12 / '13 (0xC8 or 0x7533: '13)
#   BPE entries, no 0x7532 -> SvR 2010; a 72-part crowd or 0x132-0x13F: a SvR 2011 arena already
#   DPAC / DPK8 -> a PS2 pac (the PS2 games keep every arena in one PAC/BG/BG.PAC): no PS2 arenas yet
XCOMPRESS = b'\x0f\xf5\x12\xed'
GAME_NAMES = dict((g['id'], g['name']) for g in GAMES)
GAME_NAMES['svr2011'] = "WWE SmackDown vs. Raw 2011"
WWE2K16_IDS = (0x179, 0x17A, 0x17B, 0x17D, 0x17E, 0x17F, 0x180, 0x181, 0x334, 0x3CF, 0x3D0, 0x3D1, 0x3D7, 0x3D8)
SVR2011_IDS = (0x132, 0x133, 0x134, 0x135, 0x13D, 0x13E, 0x13F)
SVR2010_IDS = (0x14A, 0x14B, 0x14C, 0x14D, 0x14E, 0x14F, 0x384, 0x385, 0x814C, 0xEA60, 0xFDE8)


def classify(ids, zlib_entries, epk8, crowd_parts, xcompressed=False):
    """(game, sure, others it could be) from a stage's entry ids - see the table above"""
    ids = set(ids)
    if 0x190 not in ids:
        if crowd_parts or 0x7922 in ids or 0xC5 in ids or 0xC6 in ids:
            return 'svr2009', True, []
        return 'svr2008', False, ['svr2009']   # (4 of 2009's 26 arenas look the same: the same convert)
    if zlib_entries and 0x7532 in ids:   # (SvR 2010 bg26 has ZLIB entries too)
        if epk8 or 0x3E6 in ids:
            return 'wwe2k15', True, []
        if crowd_parts == 72 or ids & set(WWE2K16_IDS):
            return 'wwe2k16', True, []
        if 0x3E7 in ids:   # (2K15's three plain-EPAC arenas bg00-02 are the same as 2K14's)
            return 'wwe2k14', xcompressed, [] if xcompressed else ['wwe2k15']
        return 'wwe2k16', False, ['wwe2k15']
    if 0x7532 in ids:
        if 0xC8 in ids or 0x7533 in ids or epk8:
            return 'wwe13', True, []
        return 'wwe12', 0xEE48 in ids, [] if 0xEE48 in ids else ['wwe13']
    if crowd_parts == 72 or ids & set(SVR2011_IDS):
        return 'svr2011', True, []
    return 'svr2010', bool(crowd_parts == 52 or ids & set(SVR2010_IDS)), []


def detect_pac(raw):
    """-> {'game', 'sure', 'also', 'stages': [stage numbers in the file], 'format'} or {'game': None, 'why'}"""
    fmt = []
    if raw[:4] == XCOMPRESS:
        fmt.append('xcompress')
        try:
            import arena_names as AN
            raw = AN._xdec(raw, None)
        except Exception as e:  # noqa: BLE001 - svrmod.exe missing: still say what it is
            return {'game': 'wwe13', 'sure': False, 'also': ['wwe2k14', 'wwe2k15', 'wwe2k16', 'wwe2k17'], 'stages': [],
                    'format': 'xcompress', 'why': "a WWE '13 - 2K17 pac (xcompressed; %s)" % e}
    if raw[:4] == b'HSPC':
        return {'game': 'wwe2k17', 'sure': True, 'also': [], 'stages': [], 'format': '+'.join(fmt + ['HSPC'])}
    if raw[:4] in (b'DPAC', b'DPK8'):
        return {'game': None, 'format': raw[:4].decode(), 'why':
                'a PS2 pac: the PS2 games keep every arena in one PAC/BG/BG.PAC, and PS2 arenas are not '
                'convertible yet (only the Xbox 360 games: SvR 2007 - WWE 2K17)'}
    if raw[:4] not in (b'EPAC', b'EPK8'):
        return {'game': None, 'format': raw[:4].hex(), 'why': 'not an arena pac (no EPAC / EPK8 / HSPC / DPAC header)'}
    fmt.append(raw[:4].decode())
    import moves_port
    try:
        groups = moves_port.read_pac(raw)
    except ValueError:
        return {'game': None, 'format': '+'.join(fmt), 'why': 'its table does not read (a damaged or cut file?)'}
    stages = []
    for typ, ents in groups:
        if typ[:3] == b'STG' and typ[3:4].isdigit():   # SvR 2007: STG<k>/00NN, ten stages a file
            stages += [int(n) for n, b in ents if n.isdigit() and b[:4] == b'PACH']
    if stages:
        return {'game': 'svr2007', 'sure': True, 'also': [], 'stages': sorted(stages), 'format': '+'.join(fmt)}
    stage = next(((n, b) for typ, ents in groups if typ == b'STG ' for n, b in ents if b[:4] == b'PACH'), None)
    if stage is None:
        return {'game': None, 'format': '+'.join(fmt), 'why': 'no arena (STG) entry: not an arena pac (a menu, '
                                                              'character or cage pac?)'}
    ents = svrfmt.pach_read(stage[1])
    e = dict(ents)
    crowd = 0
    if 0x4E20 in e:
        import arena_names as AN
        u = AN._unz(e[0x4E20])
        crowd = len(svrfmt.pach_read(u)) if u[:4] == b'PACH' else 0
    zl = sum(b[:4] == b'ZLIB' for _, b in ents) > len(ents) // 2
    game, sure, also = classify([i for i, _ in ents], zl, raw[:4] == b'EPK8', crowd, 'xcompress' in fmt)
    num = [int(stage[0])] if stage[0].isdigit() else []
    return {'game': game, 'sure': sure, 'also': also, 'stages': num, 'format': '+'.join(fmt)}


class LoneDisc:
    """One arena file standing in for a disc: only pac/bg/<the file> is there, so what the converters
    look for elsewhere (the menu pictures, the string tables) is missing and they fall back"""
    lone = True

    def __init__(self, path, rel):
        self.path, self.rel = os.path.abspath(path), rel.lower()

    def _is(self, p):
        return p.replace('\\', '/').strip('/').lower() == self.rel

    def exists(self, p):
        return self._is(p)

    def isdir(self, p):
        return p.replace('\\', '/').strip('/').lower() in ('', 'pac', 'pac/bg')

    def listdir(self, p):
        return [self.rel.rsplit('/', 1)[1]] if p.replace('\\', '/').strip('/').lower() == 'pac/bg' else []

    def read(self, p):
        if not self._is(p):
            raise FileNotFoundError(p)
        with open(self.path, 'rb') as f:
            return f.read()

    def size(self, p):
        return os.path.getsize(self.path)

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


def lone_stages(path, det):
    """the stage numbers in a lone file: its own (EPAC entry name), else the file name's bgNN"""
    import re
    m = re.fullmatch(r'bg(\d+)', os.path.splitext(os.path.basename(path))[0].lower())
    if det.get('stages'):
        return det['stages']
    return [int(m.group(1))] if m else []


def lone_lacks(game):
    """what a lone file can't give, in plain words, and what is used instead"""
    out = ["the arena's name and pictures: they are in the disc's menu files, not in the arena file"]
    if game in ('svr2007', 'svr2008', 'svr2009', 'svr2010', 'svr2011'):
        out.append('name: the port\'s own list by arena number (bgNN), else "Arena bgNN"; select banner: '
                   "2011's banner of the base arena (with --pac11), no own loading or VS picture (2011's stay)")
    else:
        out.append('name: bgNN (no thumbnail) - the game\'s own arena names come from its string.pac')
    if game in ('svr2007', 'svr2008', 'svr2009'):
        out.append("crowd, announce table pictures, card and the light sets it lacks: from 2011 RAW "
                   "(--pac11), the same as with the whole disc")
    out.append('nothing else: models, textures and lights are all in the arena file itself (no arena of '
               'these games keeps textures in another pac)')
    return out


def lone_items(game, path, stages):
    table = {'svr2007': SVR2007, 'svr2008': SVR2008, 'svr2009': SVR2009}.get(game)
    items = []
    for n in stages:
        show = in11 = None
        if game == 'svr2010' and n in SVR2010:
            show, in11 = SVR2010[n][1:]
        elif table and n in table:
            show, in11 = table[n]
        items.append({'key': 'bg%02d' % n, 'name': show or 'Arena bg%02d' % n, 'bg': 'bg%02d' % n, 'arena': n,
                      'size': os.path.getsize(path), 'in_2011': in11})
    if not stages:   # (a WWE 2K17 HSPC keeps no number: the file under its own name - the Mod Maker converts it whole)
        stem = os.path.splitext(os.path.basename(path))[0]
        items.append({'key': 'file', 'name': stem, 'bg': None, 'arena': None, 'size': os.path.getsize(path),
                      'in_2011': None})
    return mark_items(game, items)


def host_banner(pac11, base):
    """2011's own select banner of arena <base> (MatchHD.pac MENU/MASI), as a PIL image - or None"""
    if not pac11:
        return None
    for p in (os.path.join(pac11, 'pac', 'menu', 'MatchHD.pac'), os.path.join(pac11, 'menu', 'MatchHD.pac')):
        if os.path.exists(p):
            with open(p, 'rb') as f:
                match = f.read()
            for nm, dds in group_textures(match, 'MASI'):
                if nm == BASE_NAMES.get(base):
                    try:
                        return image(dds)
                    except Exception:  # noqa: BLE001
                        return None
    return None


def arena_cache_root():
    """where 'scan' keeps a disc's named arena list (SVR2011_ARENA_CACHE, else %LOCALAPPDATA%/SvR2011/arena_lists)"""
    return os.environ.get('SVR2011_ARENA_CACHE') or os.path.join(
        os.environ.get('LOCALAPPDATA') or tempfile.gettempdir(), 'SvR2011', 'arena_lists')


def named_list(disc, game, src, items, cache_root=None, scan=False):
    """WWE '12 - 2K17 'list' items with the game's own arena names ('name', 'bg' keeps bgNN) and, once
    'scan' has run, 'thumb' (a PNG of the arena's match intro backdrop, 320 x 160) and 'picture' (the same backdrop at
    its own size, 1024 x 512: a banner / loading picture). Names are cached per disc."""
    import arena_names as AN
    path = AN.cache_dir(os.path.abspath(cache_root or arena_cache_root()), game, src)
    cache = None if scan else AN.load_cache(path)
    if cache is None:
        progress(5, "reading the game's arena names")
        strings, base = AN.strings(disc)
        names = {str(it['arena']): AN.arena_name(strings, base, it['arena']) for it in items}
        thumbs = {}
        if scan:
            progress(55, 'reading the arena pictures')
            thumbs = {str(k): v for k, v in AN.thumbnails(disc, [it['arena'] for it in items], os.path.join(path, 'thumbs'),
                                                          progress=progress).items()}
        cache = {'game': game, 'src': os.path.abspath(src), 'base': base, 'names': names, 'thumbs': thumbs, 'scanned': scan}
        AN.save_cache(path, cache)
    for it in items:
        n = cache['names'].get(str(it['arena']))
        it['bg'] = it['key']
        if n:
            it['name'] = n
        t = cache.get('thumbs', {}).get(str(it['arena']))
        if t and os.path.exists(t):
            it['thumb'] = t
            if os.path.exists(AN.full_picture(t)):
                it['picture'] = AN.full_picture(t)
    progress(100, 'done')
    return items, cache.get('scanned', False)


FAMILY = {'svr2007': 'svr2007', 'svr2008': 'svr2008', 'svr2009': 'svr2008', 'low': 'svr2008', 'svr2010': 'svr2010',
          'svr2011': 'svr2010'}
LONE_MAGICS = (b'EPAC', b'EPK8', b'HSPC', b'DPAC', b'DPK8', XCOMPRESS)


def is_lone(src):
    """--src is one pac file (not a folder or a disc image)"""
    if not os.path.isfile(src):
        return False
    with open(src, 'rb') as f:
        return f.read(4) in LONE_MAGICS


def lone_open(a):
    """a lone --src: (game id, the detection, the stage numbers in it)"""
    with open(a.src, 'rb') as f:
        det = detect_pac(f.read())
    if det['game'] is None:
        raise BadInput('%s: %s' % (os.path.basename(a.src), det['why']))
    game = det['game'] if a.game in (None, 'auto') else a.game
    if game not in GAME_NAMES:
        raise BadInput('unknown game %r' % a.game)
    if det['sure'] and FAMILY.get(game, 'native') != FAMILY.get(det['game'], 'native'):
        raise BadInput('%s is a %s arena, not %s (leave --game out and the file says which)'
                       % (os.path.basename(a.src), GAME_NAMES[det['game']], GAME_NAMES[game]))
    return game, det, lone_stages(a.src, det)


def lone_detect_json(a):
    with open(a.src, 'rb') as f:
        det = detect_pac(f.read())
    if det['game'] is None:
        return {'ok': False, 'error': '%s: %s' % (os.path.basename(a.src), det['why']), 'format': det.get('format')}, 2
    game = det['game']
    stages = lone_stages(a.src, det)
    out = {'ok': True, 'file': os.path.abspath(a.src), 'game': game, 'game_name': GAME_NAMES[game], 'sure': det['sure'],
           'also': det['also'], 'format': det['format'], 'items': lone_items(game, a.src, stages),
           'convert': 'mod_maker' if game in NATIVE_GAMES else 'arena_port', 'lacks': lone_lacks(game), 'notes': []}
    if game == 'svr2011':
        out['notes'].append('already a SvR 2011 arena: convert keeps the file as it is')
    if not stages and game not in NATIVE_GAMES:
        out['notes'].append('no arena number in the file or its name: name it bgNN.pac or give --item bgNN')
    if game in NATIVE_GAMES:
        out['mod_maker'] = 'svrmod wwe13 <this file> <2011 host bgNN.pac> <out.pac> - as for an arena off the disc'
    if det.get('why'):
        out['notes'].append(det['why'])
    return out, 0


def main(argv):
    ap = argparse.ArgumentParser(prog='arena_port.py')
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('games')
    p = sub.add_parser('detect')   # (a lone arena pac: which game, its arena, what a lone file lacks)
    p.add_argument('--src', required=True)
    p = sub.add_parser('list')
    p.add_argument('--game')       # (required for a disc; a lone pac: optional, the file says)
    p.add_argument('--src', required=True)
    p.add_argument('--pac11')
    p.add_argument('--cache')
    p = sub.add_parser('scan')   # (WWE '12 - 2K17: read the disc once - names and pictures - into the cache)
    p.add_argument('--game', required=True)
    p.add_argument('--src', required=True)
    p.add_argument('--cache')
    p = sub.add_parser('convert')
    p.add_argument('--game')
    p.add_argument('--src', required=True)
    p.add_argument('--item')       # (required for a disc; a lone pac: its own arena)
    p.add_argument('--out', required=True)
    p.add_argument('--base', type=int)
    p.add_argument('--name')
    p.add_argument('--pac11')
    for k in ('list', 'scan', 'convert'):   # (the source game's DLC packages: read beside the disc)
        sub.choices[k].add_argument('--dlc')
    try:
        a = ap.parse_args(argv)
    except SystemExit:
        print(json.dumps({'ok': False, 'error': 'bad arguments'}))
        return 2
    if getattr(a, 'dlc', None):
        os.environ['SVR2011_PORT_DLC'] = a.dlc
    if a.cmd == 'games':
        print(json.dumps(GAMES))
        return 0
    try:
        if not os.path.exists(a.src):
            raise BadInput('--src %s: not found' % a.src)
        lone = is_lone(a.src)
        if a.cmd == 'detect':
            if not lone:
                raise BadInput('--src %s: not a pac file (detect takes one arena pac)' % a.src)
            out, code = lone_detect_json(a)
            print(json.dumps(out))
            return code
        lone_note = []
        if lone:
            if a.cmd == 'scan':
                raise BadInput('scan reads a whole disc (names and pictures); a lone arena pac has neither: use list')
            gid, det, stages = lone_open(a)
            if a.cmd == 'list':
                print(json.dumps(lone_items(gid, a.src, stages)))
                return 0
            if gid in NATIVE_GAMES:
                n = None
            elif a.item:
                try:
                    n = int(a.item[2:]) if a.item.lower().startswith('bg') else int(a.item)
                except ValueError:
                    raise BadInput('item %r: expected bgNN' % a.item)
                if stages and n not in stages:
                    raise BadInput('item %r is not in this file (it holds %s)'
                                   % (a.item, ', '.join('bg%02d' % k for k in stages)))
            elif stages:
                n = stages[0]
            else:
                raise BadInput('no arena number in %s or its name: name the file bgNN.pac or give --item bgNN'
                               % os.path.basename(a.src))
            maybe = '' if det['sure'] or not det['also'] else \
                ' - told from the file; it may be ' + ' / '.join(GAME_NAMES[g] for g in det['also'])
            lone_note = ['a lone arena file: %s%s' % (GAME_NAMES[gid], maybe)] + \
                ['lone file - ' + x for x in lone_lacks(gid)]
            if gid in NATIVE_GAMES:
                rep = {'ok': False, 'out': None, 'opens': 'arena', 'warnings': [], 'dropped': [], 'game': gid,
                       'notes': ["WWE '12 - 2K17 arenas are converted by the Mod Maker itself: svrmod wwe13 "
                                 "<this file> <2011 host bgNN.pac> <out.pac>"] + lone_note}
                print(json.dumps(rep))
                return 3
            disc = LoneDisc(a.src, svr2007_path(n) if gid == 'svr2007' else 'pac/bg/bg%02d.pac' % n)
            item, game_id = 'bg%02d' % n, gid
        else:
            if not a.game or (a.cmd == 'convert' and not a.item):
                raise BadInput('--game%s needed for a disc or game folder'
                               % (' and --item' if a.cmd == 'convert' else ''))
            if a.game not in GAME_NAMES or a.game == 'svr2011':
                raise BadInput('unknown game %r' % a.game)
            disc = discfs.open(a.src)
            if a.game in NATIVE_GAMES:   # (the game's DLC packages beside the disc: --dlc)
                import dlc_src
                disc = dlc_src.view(disc)
            item, game_id = getattr(a, 'item', None), a.game  # (list / scan have no --item)
        with disc:
            if not disc.isdir('pac/bg'):
                raise BadInput('the game files have no pac/bg folder (an incomplete extract? use the whole disc '
                               'folder, the ISO or one arena pac)')
            if a.cmd in ('list', 'scan'):
                lists = {'svr2010': svr2010_list, 'svr2008': svr2008_list,
                         'svr2009': lambda d: svr2008_list(d, SVR2009), 'low': lambda d: svr2008_list(d, LOW),
                         'svr2007': lambda d: svr2008_list(d, SVR2007)}
                items = lists.get(game_id, bg_list)(disc)
                if game_id in NATIVE_GAMES:
                    items, _ = named_list(disc, game_id, a.src, items, a.cache, scan=a.cmd == 'scan')
                    items = dlc_files(disc, game_id, a.src, items, a.cache)
                print(json.dumps(mark_items(game_id, items)))
                return 0
            if game_id not in FAMILY:
                msg = ("WWE '12 - 2K17 arenas are converted by the Mod Maker itself (svrmod wwe13)" if game_id in NATIVE_GAMES
                       else '%s arenas are not convertible yet (research)' % GAME_NAMES.get(game_id, game_id))
                rep = {'ok': False, 'out': None, 'opens': 'arena', 'warnings': [], 'dropped': [], 'notes': [msg]}
                print(json.dumps(rep))
                return 3
            os.makedirs(a.out, exist_ok=True)
            if game_id in ('svr2007', 'svr2008', 'svr2009', 'low'):
                rep = svr2008_convert(disc, item, a.out, a.pac11, a.base, a.name, game_id)
            else:
                rep = svr2010_convert(disc, item, a.out, a.base, a.name, a.pac11, '2011' if game_id == 'svr2011' else '2010')
                if game_id == 'svr2011':
                    rep['notes'].insert(0, 'already a SvR 2011 arena: the file is kept as it is')
            if lone_note:
                rep['notes'] = lone_note + rep['notes']
                rep['game'] = game_id
            with open(os.path.join(a.out, 'report.json'), 'w') as f:
                json.dump(rep, f, indent=1)
            print(json.dumps(rep))
            return 0
    except BadInput as e:
        print(json.dumps({'ok': False, 'error': str(e)}))
        return 2
    except Exception as e:  # noqa: BLE001 - reported to the Mod Maker
        log('failed: %r' % e)
        print(json.dumps({'ok': False, 'error': str(e), 'out': None, 'opens': 'arena', 'warnings': [], 'dropped': [],
                          'notes': []}))
        return 3


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
