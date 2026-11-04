"""An SvR 2010 superstar as an SvR 2011 superstar mod (.svrmod), built the way
the bundled Jeff Hardy mod was.

    python svr10_superstar.py <2010 id> <out.svrmod> [options]
    python svr10_superstar.py survey [--out roster.tsv]

Options:
    --pac10 DIR     SvR 2010 "pac" folder  (needed)
    --pac11 DIR     SvR 2011 "pac" folder  (default ..\\..\\Game Files\\pac from this script's folder)
    --base N        the 2011 superstar the mod starts from (default: chosen, see below)
    --model 10|11   take the model from that game (default: 2011's own chNNN.pac when 2011 ships
                    one for the id, else 2010's through svr10_char.py)
    --entrance N    entrance number (default: the 2011 entrance whose name matches, else the base's)
    --call N        Created Superstar nickname for the commentary (default: table below)
    --no-moves      keep the base's moves (no moves.txt / move pack)
    --height S      height=S in the manifest: the model's size from its feet, 0.80-1.25 (default: left out)
    --verify        let svr10_moves.py write the full patched pacs and verify them (slow, ~1 GB)
    --work DIR      working folder (default: <out>.work next to the output)

Both games are only read. ffmpeg (PATH or C:\\ffmpeg\\bin) converts the 2010 theme.

What goes in the mod (docs/SUPERSTAR_MODS.md, docs/MOVE_PACKS.md):
  ch.pac             2010 chNNN.pac converted by svr10_char.py (2011 face animation, 256 bust as
                     portrait), only the kind-2 (wrestler) model entries; or 2011's own chNNN.pac
  render.dds         2010 DLC_HD.pac SSFB/<id> (512, full body), render_small.dds = SSFC (256 bust);
                     unpacked DDS (the port BPE-compresses them itself)
  theme.mp3          2010 sound/music1.yaf record <music id>*100 (XMA2 -> mp3 256k)
  movie.bik          2010 movies/titantron/<movie id>.bik (320x320 Bink, used as is)
  ratings=           2010 CHAR/DAT +0..+6 minus 1 (2011 stores display-1; 2010's 8th value dropped)
  abilities=         2010 abilities that 2011 still uses (1,7,9,11,12,14,19,20,22,23,24), max 5
  moves=moves.txt    the 2010 move set mapped slot by slot onto the base's 2011 profile
                     (2010 -> 2011 slot map MAP, category bits per slot); every 2010 move whose
                     motion 2011 lacks is ported (svr10_moves.py build -> movepack.py -> moves/)
  entrance=          the 2011 evt/Nyujyo*.pac entrance with the same name (Jeff Hardy -> 535)
  announcer=         NAME when 2011 RA.pck has Play_RA_JR/TC_SSN_<NAME>_0 (real recordings)
  call=              nickname for the commentary, unless 2011 has a Comm_<id> bank and base = id

Base: the 2011 playable superstar (SSFA render, selectable) of the same gender and weight
class, closest body scale (CHAR/DAT +28; the mod inherits the base's) and most move ids in
common with the 2010 profile. BASE_OVERRIDE pins a few (Jeff -> his brother Matt).
"""
import collections
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ch_tool  # noqa: E402
import svrfmt  # noqa: E402
import svr10_char  # noqa: E402
import svr10_moves  # noqa: E402
import movepack  # noqa: E402

PAC11 = os.path.join(os.path.dirname(os.path.dirname(HERE)), 'Game Files', 'pac')

# --------------------------------------------------------------------------- tables

# 2010 CHAR/PRO byte offset -> 2011 byte offset (research svr10/moves/promap.py)
def _rng(a0, a1, b0):
    return [(a, b0 + (a - a0)) for a in range(a0, a1 + 2, 2)]


MAP = (_rng(0x00, 0x04, 0x00) + _rng(0x06, 0x10, 0x06) + _rng(0x12, 0x18, 0x12) + [(0x1e, 0x1e), (0x20, 0x20), (0x22, 0x22), (0x24, 0x24)]
       + _rng(0x26, 0x38, 0x26) + _rng(0x3a, 0x4c, 0x4e) + [(0x4e, 0x62), (0x50, 0x64), (0x52, 0x66), (0x54, 0x68)]
       + _rng(0x56, 0x64, 0x6a) + _rng(0x66, 0x6c, 0x7a) + _rng(0x6e, 0x74, 0x84) + [(0x76, 0x8c)] + _rng(0x78, 0x7e, 0x8e)
       + [(0x80, 0x96)] + _rng(0x82, 0x88, 0x98) + [(0x8a, 0xa0)] + _rng(0x8c, 0x92, 0xa2) + [(0x94, 0xaa)]
       + _rng(0x96, 0x9c, 0xac) + _rng(0x9e, 0xa4, 0xb4) + _rng(0xa6, 0xac, 0xbc)
       + [(0xb4, 0xcc), (0xb6, 0xce), (0xb8, 0xd0), (0xba, 0xd2), (0xc0, 0xd8), (0xc2, 0xda), (0xc4, 0xdc), (0xc6, 0xde),
          (0xcc, 0xe4), (0xce, 0xe6), (0xd0, 0xe8), (0xd2, 0xea), (0xd8, 0xf0), (0xde, 0xf6), (0xe4, 0xfc)]
       + [(0xe6, 0xfe), (0xec, 0x106), (0xf0, 0x10a), (0xf4, 0x114), (0xf6, 0x116), (0xfc, 0x11e), (0x102, 0x122),
          (0x106, 0x12a), (0x108, 0x12e)] + _rng(0x10a, 0x110, 0x150) + [(0x112, 0x158)] + _rng(0x114, 0x12a, 0x15a)
       + [(0x12c, 0x172), (0x12e, 0x174), (0x130, 0x176), (0x132, 0x178), (0x134, 0x17a), (0x136, 0x17c), (0x138, 0x17e),
          (0x13a, 0x180), (0x13c, 0x182), (0x13e, 0x184), (0x140, 0x186)] + _rng(0x142, 0x146, 0x188) + _rng(0x148, 0x156, 0x18e)
       + [(0x15c, 0x1a2), (0x15e, 0x1a4), (0x160, 0x1a6), (0x162, 0x1a8)] + _rng(0x164, 0x16a, 0x1aa) + _rng(0x16c, 0x172, 0x1b6))
MARKERS = {10900, 1001, 1101, 1051, 1151, 10280, 10290, 1290, 1380, 1381, 1382, 1383, 1384, 1385, 800, 810, 100, 101, 102, 103,
           900, 1700, 1775, 9900, 8370, 8371, 8360, 8361, 8380, 8381, 0}
SEATED_TOP = {13, 14}                            # move situations: opponent seated on the top turnbuckle
SECTIONS = [(0x7a, 0xc2), (0x104, 0x158)]      # grapple and corner/top-rope sections: a move may move within them
ABILITIES_2011 = {1, 7, 9, 11, 12, 14, 19, 20, 22, 23, 24}
# Created Superstar nicknames for the commentary (Mod Maker list, 0..83)
CALL = {111: 71, 114: 48, 118: 63, 127: 43, 136: 44, 152: 68, 155: 54, 157: 72, 162: 75, 166: 61, 168: 54, 172: 48,
        189: 2, 207: 78, 209: 48, 213: 48, 214: 46, 227: 70, 302: 69, 303: 48}
BASE_OVERRIDE = {111: 112}                       # Jeff Hardy -> Matt Hardy (brother: same build, tag partner)
RA_ALIAS = {136: ['TEDDIBIASESR'], 166: ['KHALI'], 162: ['MRKENNEDY'], 274: ['HURRICANE']}
# 2011-only characters: name, and entrance (274: 549 "Hurricane" was made for 2011 with his costumed
# model; his 2011 profile still points at 510 "Glegory Helms", the 2008-2010 out-of-costume one)
NAME_OVERRIDE = {274: 'The Hurricane'}
ENTRANCE_OVERRIDE = {274: 549}
MANAGERS_ETC = {147, 150, 153, 186, 187, 188, 195, 231, 251, 255, 287, 288, 289, 296, 301}


def u16s(b, n=None):
    n = len(b) // 2 if n is None else n
    return list(struct.unpack_from('<%dH' % n, b, 0))


def norm(s):
    s = s.lower().replace('_', ' ').strip()
    s = re.sub(r'^the ', '', s)
    return re.sub(r'[^a-z0-9]', '', s)


# --------------------------------------------------------------------------- game data

class Game:
    """What one game knows about characters: CHAR/DAT records, CHAR/PRO profiles, renders,
    entrances, titantrons, move records."""

    def __init__(self, pac, year):
        self.pac, self.year = pac, year
        h, g, t = svrfmt.epac_read(open(os.path.join(pac, 'ch', 'chEtc.pac'), 'rb').read())
        ch = {(ty, n): b for ty, es in g for n, b in es}

        def pach(b):
            return {i: ch_tool.unpack(d) for i, d in svrfmt.pach_read(ch_tool.unpack(b))}
        self.dat = {i: r[4:] for i, r in pach(ch[(b'CHAR', b'DAT ')]).items()}
        self.pro = pach(ch[(b'CHAR', b'PRO ')])
        h, g, t = svrfmt.epac_read(open(os.path.join(pac, 'DLC_HD.pac'), 'rb').read())
        self.ssf = {(ty.decode(), int(n)): b for ty, es in g if ty in (b'SSFA', b'SSFB', b'SSFC') for n, b in es}
        self._evp = None
        self._waze = None

    def name(self, i):
        r = self.dat.get(i)
        return r[34:101].split(b'\0')[0].decode('latin1') if r else ''

    def short(self, i):
        r = self.dat.get(i)
        return r[170:207].split(b'\0')[0].decode('latin1') if r else ''

    def named(self, i):
        return self.name(i) not in ('', '0')

    def chpac(self, i):
        p = os.path.join(self.pac, 'ch', 'ch%d.pac' % i)
        return p if os.path.exists(p) else None

    def titantron(self, n):
        p = os.path.join(os.path.dirname(self.pac), 'movies', 'titantron', '%03d.bik' % n)
        return p if os.path.exists(p) else None

    def entrance_block(self, i):
        """-> (music, movie, entrance, alt) of attire 1"""
        off = 0x178 if self.year == 10 else 0x1c4
        return struct.unpack_from('<4H', self.pro[i], off)

    def evp(self):
        """{entrance number: name} from evt/Nyujyo*.pac (EPK8, count = u16/4; EVP name at +0x22)"""
        if self._evp is None:
            self._evp = {}
            d = os.path.join(self.pac, 'evt')
            for f in sorted(os.listdir(d)):
                if not f.startswith('Nyujyo'):
                    continue
                data = open(os.path.join(d, f), 'rb').read()
                q = 0x800
                while q < 0x4000:
                    typ = data[q:q + 4]
                    if typ == b'\0\0\0\0':
                        break
                    cnt = struct.unpack_from('<H', data, q + 4)[0] // 4
                    q += 12
                    for _ in range(cnt):
                        nm = data[q:q + 8].replace(b'\0', b'').strip()
                        sec, sz = struct.unpack_from('<II', data, q + 8)
                        q += 16
                        if typ[:3] != b'EVP' or not nm.isdigit() or int(nm) >= 1000:
                            continue
                        b = ch_tool.unpack(data[0x4000 + sec * 0x800:0x4000 + sec * 0x800 + sz * 0x100])
                        self._evp[int(nm)] = b[0x22:0x42].split(b'\0')[0].decode('latin1').strip()
        return self._evp

    def waze(self):
        """{move id: (16-byte category bits, name)} from misc.pac MOVS/WAZE"""
        if self._waze is None:
            h, g, t = svrfmt.epac_read(open(os.path.join(self.pac, 'misc.pac'), 'rb').read())
            raw = ch_tool.unpack(dict(((ty, n), b) for ty, es in g for n, b in es)[(b'MOVS', b'WAZE')])
            st = raw.index(b'* test motion *') - 0x10
            self._waze, self._situation = {}, {}
            for k in range((len(raw) - st) // 160):
                r = raw[st + 160 * k:st + 160 * (k + 1)]
                if len(r) == 160 and r.strip(b'\0'):
                    mid = struct.unpack_from('<H', r, 0x90)[0]
                    self._waze.setdefault(mid, (r[:16], r[0x10:0x50].split(b'\0')[0].decode('utf-8', 'replace')))
                    self._situation.setdefault(mid, r[0x73])
        return self._waze

    def situation(self):
        """{move id: the situation it starts from (WAZE +0x73)}, e.g. 10/11 at the corner (2010's
        top-rope moves lift the opponent up themselves), 13/14 the opponent already seated on the
        top turnbuckle facing out / in (2011)"""
        self.waze()
        return self._situation


# ------------------------------------------------------------ Wwise (2011 sound/*.pck)

def fnv(s):
    h = 2166136261
    for c in s.lower().encode():
        h = (h * 16777619) & 0xffffffff
        h ^= c
    return h


class Pck:
    def __init__(self, path):
        self.f = open(path, 'rb')
        hd = self.f.read(0x400000)
        hs, ver, lm, bl, sl = struct.unpack_from('>IIIII', hd, 4)

        def lut(p):
            n = struct.unpack_from('>I', hd, p)[0]
            return [struct.unpack_from('>IIIIII', hd, p + 4 + 24 * i) for i in range(n)]
        p = 0x18 + lm
        self.banks = lut(p)
        self.streams = {s[0]: s for s in lut(p + bl)}
        self._objs = None

    def objs(self):
        if self._objs is None:
            self._objs = {}
            for bid, blk, hi, size, off, lang in self.banks:
                self.f.seek(off * blk)
                d = self.f.read(size)
                p = 0
                while p + 8 <= len(d):
                    tag, ln = struct.unpack_from('>4sI', d, p)
                    if tag == b'HIRC':
                        n = struct.unpack_from('>I', d, p + 8)[0]
                        q = p + 12
                        for _ in range(n):
                            t, sz, oid = struct.unpack_from('>III', d, q)
                            self._objs[oid] = (t, d[q + 8:q + 8 + sz])
                            q += 8 + sz
                    p += 8 + ln
        return self._objs

    def has_event(self, name):
        o = self.objs().get(fnv(name))
        return bool(o and o[0] == 4)

    def bank_exists(self, name):
        return any(b[0] == fnv(name) for b in self.banks)

    def event_streams(self, name):
        """event -> play actions -> ... -> sounds: [stream source id]"""
        objs = self.objs()
        ev = objs.get(fnv(name))
        if not ev or ev[0] != 4:
            return None
        d = ev[1]
        out = []
        for k in range(struct.unpack_from('>I', d, 4)[0]):
            a = objs.get(struct.unpack_from('>I', d, 8 + 4 * k)[0])
            if not a or a[0] != 3:
                continue
            stack, seen = [struct.unpack_from('>I', a[1], 8)[0]], set()
            while stack:
                o = stack.pop()
                if o in seen or o not in objs:
                    continue
                seen.add(o)
                t, do = objs[o]
                if t == 2:
                    out.append(struct.unpack_from('>I', do, 20)[0])
                else:
                    key = struct.pack('>I', o)
                    stack += [k2 for k2, (t2, d2) in objs.items() if k2 != o and t2 in (2, 5, 6, 7, 9) and key in d2[4:60]]
        return out

    def stream_users(self):
        c = collections.Counter()
        for t, d in self.objs().values():
            if t == 2:
                c[struct.unpack_from('>I', d, 20)[0]] += 1
        return c


class Sound11:
    def __init__(self, pac11):
        self.dir = os.path.join(os.path.dirname(pac11), 'sound')
        self._p = {}

    def pck(self, n):
        if n not in self._p:
            self._p[n] = Pck(os.path.join(self.dir, n))
        return self._p[n]

    def announcer(self, cands):
        """first NAME with real name calls by both announcers (else by either) -> (NAME, [clips])"""
        ra = self.pck('RA.pck')
        best = None
        for n in cands:
            got = ['%s_%s_%d' % (w, k, v) for k in ('SSN', 'SSP', 'WITHSSN') for v in range(12) for w in ('JR', 'TC')
                   if ra.has_event('Play_RA_%s_%s_%s_%d' % (w, k, n, v))]
            both = 'JR_SSN_0' in got and 'TC_SSN_0' in got
            if both:
                return n, got
            if got and best is None and ('JR_SSN_0' in got or 'TC_SSN_0' in got):
                best = (n, got)
        return best or (None, [])

    def commentary(self, cid):
        return self.pck('Comm.pck').bank_exists('Comm_%04d' % cid)

    def theme(self, music):
        m = self.pck('Music.pck')
        s = m.event_streams('Play_MUS_%04d_0_0' % music)
        if not s:
            return None
        if not hasattr(self, '_users'):
            self._users = m.stream_users()
        return 'real' if self._users[s[0]] < 5 else 'placeholder (shared stream, no theme)'


# --------------------------------------------------------------------------- 2010 audio

def yaf_table(fn):
    with open(fn, 'rb') as f:
        d = f.read(0x40000)
        n, align, ver, rs, to = struct.unpack_from('<IIIII', d, 0x18)
        if to + rs * n > len(d):
            f.seek(0)
            d = f.read(to + rs * n)
    return {r[2]: (r[1], r[0]) for r in (struct.unpack_from('<' + 'I' * (rs // 4), d, to + rs * i) for i in range(n))}


def ffmpeg():
    for p in (shutil.which('ffmpeg'), r'C:\ffmpeg\bin\ffmpeg.exe'):
        if p and os.path.exists(p):
            return p
    raise SystemExit('ffmpeg not found')


def theme10(pac10, music, out_mp3):
    """2010 music1.yaf record music*100 -> mp3. -> record id or None"""
    fn = os.path.join(os.path.dirname(pac10), 'sound', 'music1.yaf')
    t = yaf_table(fn)
    rid = music * 100
    if rid not in t:
        return None
    off, size = t[rid]
    with open(fn, 'rb') as f:
        f.seek(off)
        data = f.read(size)
    raw = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(out_mp3))), 'theme_%d.xma.wav' % rid)   # outside mod/
    open(raw, 'wb').write(data)
    # Mod themes play through the port's XMP player, about 7 dB louder than the game's own Wwise
    # themes: bring each to THEME_LUFS integrated with one linear gain (dynamics kept).
    src = loudness(raw)
    gain = THEME_LUFS - src
    subprocess.run([ffmpeg(), '-hide_banner', '-loglevel', 'error', '-y', '-i', raw, '-af', 'volume=%.2fdB' % gain,
                    '-c:a', 'libmp3lame', '-b:a', '256k', out_mp3], check=True)
    try:
        os.remove(raw)
    except OSError:   # a virus scanner may still hold it; it is left in the work folder, not the mod
        pass
    theme10.last = (src, gain, loudness(out_mp3))
    return rid


THEME_LUFS = -24.0


def loudness(path):
    """integrated loudness (LUFS) by ffmpeg's EBU R128 meter"""
    r = subprocess.run([ffmpeg(), '-hide_banner', '-nostats', '-i', path, '-af', 'ebur128', '-f', 'null', '-'],
                       capture_output=True, text=True, errors='replace')
    m = re.findall(r'I:\s+(-?[0-9.]+) LUFS', r.stderr)
    if not m:
        raise SystemExit('no loudness for ' + path)
    return float(m[-1])


# --------------------------------------------------------------------------- model

def model_from_2010(pac10, cid, pac11, out):
    """svr10_char.convert, plus what the 2010 non-wrestler models (announcers, commentators,
    Shane, Tazz) need to stand in as wrestlers:
      - only the wrestler models (EMD kind 2) are kept; commentators also carry a seated kind-6
        model. A pac with no kind 2 (Lilian Garcia: kind 0, the ring-announcer kind) has its
        kind-0 model renamed to kind 2 [guess: same JBOY format, fewer parts].
      - children every 2011 model has but these lack are added: 0x50 (1648-byte table, from
        2011 ch104) and the 0xc8/0xc9 portrait (the 2010 SSFC bust). 0x64 (face animation) is
        only replaced, never added: 2011's own managers (Stephanie, ch147) ship without one."""
    src = os.path.join(pac10, 'ch', 'ch%d.pac' % cid)
    face = svr10_char.generic_face(pac11)
    c104 = [b for t, n, b in svr10_char.epk8_entries(os.path.join(pac11, 'ch', 'ch104.pac')) if t == b'EMD '][0]
    t50 = dict(svrfmt.pach_read(ch_tool.unpack(c104)))[0x50]
    bust = svr10_char.dlc_render(pac10, 'SSFC', cid)
    allents = svr10_char.epk8_entries(src)
    kinds = {n[7:8] for t, n, b in allents if t == b'EMD '}
    want = b'2' if b'2' in kinds else b'0'
    entries, kept, notes = [], [], []
    for typ, name, blob in allents:
        if typ == b'EMD ' and want == b'2' and name[7:8] == b'4':   # (entrance gear: the whole wrestler in a coat / hat)
            if ch_tool.unpack(blob)[:4] == b'PACH':
                kids = svrfmt.pach_read(ch_tool.unpack(blob))
                have = {i for i, d in kids}
                drop = {0x64, 0xc8, 0xc9, 0x7530, 0x7531, 0x7532}   # (as 2011's own gear models: none of these)
                new = [(i, d) for i, d in kids if i not in drop] + ([(0x50, t50)] if 0x50 not in have else [])
                blob = svrfmt.pach_write(sorted(new, key=lambda x: x[0]))
                notes.append('%s: entrance gear' % name.decode())
            entries.append((typ, name, blob))
            kept.append(name.decode())
            continue
        if typ == b'EMD ':
            if name[7:8] != want:
                continue
            if want != b'2':
                notes.append('%s renamed to kind 2' % name.decode())
                name = name[:7] + b'2'
            if ch_tool.unpack(blob)[:4] == b'PACH':
                kids = svrfmt.pach_read(ch_tool.unpack(blob))
                have = {i for i, d in kids}
                new = [(i, face if i == 0x64 else bust if i in (0xc8, 0xc9) else data) for i, data in kids]
                add = [(0x50, t50)] if 0x50 not in have else []
                add += [(i, bust) for i in (0xc8, 0xc9) if i not in have]
                if add:
                    notes.append('%s: added %s' % (name.decode(), ' '.join('0x%x' % i for i, d in add)))
                new = sorted(new + add, key=lambda x: x[0])
                blob = svrfmt.pach_write(new)
        entries.append((typ, name, blob))
        kept.append(name.decode())
    if not any(t == b'EMD ' for t, n, b in entries):
        raise SystemExit('ch%d.pac has no model' % cid)
    # svr10_char.epk8_write lays entries out by the template's index: rewrite its rows
    tmpl = bytearray(open(src, 'rb').read(0x4000))
    p = 0x800
    while p < 0x4000 and tmpl[p:p + 4] != b'\0\0\0\0':
        typ = bytes(tmpl[p:p + 4])
        cnt = struct.unpack_from('<H', tmpl, p + 4)[0]
        q = p + 12
        rows = [n + bytes(8) for t, n, b in entries if t == typ]
        rows += [bytes(16)] * (cnt - len(rows))
        tmpl[q:q + 16 * cnt] = b''.join(rows)
        p = q + 16 * cnt
    tp = out + '.tmpl'
    open(tp, 'wb').write(bytes(tmpl))
    open(out, 'wb').write(svr10_char.epk8_write(tp, entries))
    os.remove(tp)
    return kept + notes


# --------------------------------------------------------------------------- moves

class Moves:
    def __init__(self, g10, g11, pac10, pac11, work):
        self.g10, self.g11 = g10, g11
        self.w10, self.w11 = g10.waze(), g11.waze()
        self.ctx = None
        self.pac10, self.pac11, self.work = pac10, pac11, work
        cache = os.path.join(work, 'cache')
        os.makedirs(cache, exist_ok=True)
        idx11 = svr10_moves.bank_index(pac11, os.path.join(cache, 'idx11.pkl'))
        idx10 = svr10_moves.bank_index(pac10, os.path.join(cache, 'idx10.pkl'))
        self.motion11 = {k[0] for p, v in idx11.items() if not p.startswith('MOTP') for k in v[1]}
        self.motion10 = {k[0] for p, v in idx10.items() if not p.startswith('MOTP') for k in v[1]}
        # YMBs (ADPCM) moves - taunts (BASE), stances, submissions - are not portable by svr10_moves.py
        self.ymbs10 = {k[0] for p, v in idx10.items() if not p.startswith('MOTP') and v[0] == 'YMBs' for k in v[1]}
        # each 2011 slot's category bits: those >=90% of the real roster's moves in that slot have
        real = [c for c in g11.pro if 100 <= c < 9000 and g11.named(c) and not g11.name(c).upper().startswith('MOTION')]
        self.need = {}
        for s in range(0, 0x1c0, 2):
            fl = [int.from_bytes(self.w11[m][0], 'little') for m in (u16s(g11.pro[c], 224)[s // 2] for c in real) if m in self.w11]
            cnt = collections.Counter(b for x in fl for b in range(128) if x >> b & 1)
            self.need[s] = {b for b, n in cnt.items() if fl and n >= 0.9 * len(fl)} - {121, 124, 127}
        # slots for an opponent seated on the top turnbuckle (situations 13/14, 0x140-0x146): 2011
        # puts him there first (23100 lifts him, then raises his base by 1000 units, event op 79),
        # so the move must start from the seat. A 2010 top-rope move (situation 10/11) lifts him
        # itself from the mat and, played there, lifts him a body-height above the turnbuckle
        # (Sabu's Super Hurricanrana 5735). Such a slot takes only moves of its situation.
        self.sit11 = g11.situation()
        self.sit_need = {}
        for s in range(0, 0x1c0, 2):
            ss = [self.sit11[m] for m in (u16s(g11.pro[c], 224)[s // 2] for c in real) if m in self.sit11]
            top = collections.Counter(ss).most_common(1)
            if top and top[0][0] in SEATED_TOP and top[0][1] >= 0.9 * len(ss):
                self.sit_need[s] = top[0][0]

    def generic(self, cid):
        """the 2010 default profile shared by the non-wrestlers (announcers, Shane), or a
        non-wrestler's (all ratings 50: Lilian Garcia)"""
        a = self.g10.pro[cid][:0x174]
        if all(x == 50 for x in self.g10.dat[cid][:8]):
            return True
        return sum(1 for c in self.g10.pro if c != cid and self.g10.named(c) and self.g10.pro[c][:0x174] == a) >= 2

    def missing(self, cid):
        j = u16s(self.g10.pro[cid], 0x174 // 2)
        out = []
        for s10, s11 in MAP:
            m = j[s10 // 2]
            if (m not in MARKERS and m not in self.motion11 and m in self.motion10 and m not in self.ymbs10
                    and m in self.w11 and m not in out):
                out.append(m)
        return out

    def assign(self, cid, base, ported_flags):
        """-> (2011 move area list, rows, extra bits {id: [bits]})"""
        j = u16s(self.g10.pro[cid], 0x174 // 2)
        t = u16s(self.g11.pro[base], 224)
        out, used, rows, extra = list(t), set(), [], {}

        def fbits(m):
            if m in ported_flags:
                x = int.from_bytes(ported_flags[m], 'little')
            else:
                x = int.from_bytes(self.w11[m][0], 'little') if m in self.w11 else 0
            return {b for b in range(128) if x >> b & 1}

        def has(m):
            return m in self.motion11 or m in ported_flags

        def fits(m, s):
            # the slot's category bits, and its situation where it needs one
            return self.need[s] <= fbits(m) and (s not in self.sit_need or self.sit11.get(m) == self.sit_need[s])

        def stem(n):
            return re.sub(r'\s*\d+$', '', n.strip()).lower()

        def substitute(m, s11):
            # a 2011 move of the same name stem ("Snap Jab 1" for "Snap Jab 2") that fits the slot
            st = stem(self.w10.get(m, (b'', ''))[1] or self.w11.get(m, (b'', ''))[1])
            c = [k for k, (f, n) in self.w11.items() if k != m and has(k) and st and stem(n) == st
                 and self.need[s11] and fits(k, s11)]
            return min(c, key=lambda k: abs(k - m)) if c else None

        def relocate(m, s11):
            for lo, hi in SECTIONS:
                if lo <= s11 <= hi:
                    for s in range(lo, hi + 2, 2):
                        if s not in used and self.need[s] and fits(m, s) and t[s // 2] not in MARKERS:
                            return s
            return None
        for s10, s11 in MAP:
            m = j[s10 // 2]
            nm = self.w10.get(m, (b'', ''))[1]
            if m in MARKERS:
                continue
            wrong_sit = s11 in self.sit_need and self.sit11.get(m) != self.sit_need[s11]
            if has(m) and fits(m, s11) and s11 not in used:
                out[s11 // 2] = m
                used.add(s11)
                v = 'ok' + (' (ported)' if m in ported_flags else '')
            elif has(m):
                r = relocate(m, s11)
                if r is not None:
                    out[r // 2] = m
                    used.add(r)
                    v = 'moved to 0x%03x' % r
                    s11 = r
                else:
                    sub = substitute(m, s11) if s11 not in used else None
                    why = ('starts at the corner (situation %s), the slot from the top turnbuckle (%d)'
                           % (self.sit11.get(m), self.sit_need[s11]) if wrong_sit
                           else 'category bits %s missing' % sorted(self.need[s11] - fbits(m)))
                    if m in ported_flags and not wrong_sit:
                        extra[m] = sorted(set(extra.get(m, [])) | (self.need[s11] - fbits(m)))
                    if sub is not None and (m not in ported_flags or wrong_sit):
                        out[s11 // 2] = sub
                        used.add(s11)
                        v = '%s -> substitute %d %s' % (why, sub, self.w11[sub][1])
                    else:
                        v = '%s -> base move kept' % why
            else:
                sub = substitute(m, s11) if s11 not in used else None
                if sub is not None:
                    out[s11 // 2] = sub
                    used.add(s11)
                    v = 'not portable (%s) -> substitute %d %s' % ('YMBs motion' if m in self.ymbs10 else 'no motion', sub, self.w11[sub][1])
                else:
                    v = 'not portable (%s) -> base move kept' % ('YMBs motion' if m in self.ymbs10 else 'no motion')
            rows.append((s10, s11, m, nm, v, out[s11 // 2]))
        return out, rows, extra

    def port(self, ids, outdir, extra_bits, verify):
        if self.ctx is None:
            print('loading both games\' m.pac / mpsp.pac / misc.pac ...')
            self.ctx = svr10_moves.Ctx(self.pac10, self.pac11, self.work)
        ctx = self.ctx
        ctx.out, ctx.log, ctx.extra_bits = outdir, [], dict(extra_bits)
        os.makedirs(outdir, exist_ok=True)
        if not verify:   # the move pack is all a mod needs: skip writing the 1 GB of patched pacs
            saved = (svr10_moves.svrfmt, svr10_moves.refresh_arc)

            class NoWrite:
                def __getattr__(self, k):
                    return getattr(svrfmt, k)

                @staticmethod
                def epac_write(*a, **k):
                    return b''
            svr10_moves.svrfmt, svr10_moves.refresh_arc = NoWrite(), (lambda *a, **k: None)
        try:
            svr10_moves.build(ctx, list(ids))
        finally:
            if not verify:
                svr10_moves.svrfmt, svr10_moves.refresh_arc = saved
                for f in ('m.pac', 'mpsp.pac', 'misc.pac'):
                    if os.path.exists(os.path.join(outdir, f)):
                        os.remove(os.path.join(outdir, f))
        ok = None
        if verify:
            ok = svr10_moves.verify(self.pac11, outdir)
        man = json.load(open(os.path.join(outdir, 'movepack', 'manifest.json')))
        return man, ok


# --------------------------------------------------------------------------- decisions

def candidates_11(g11):
    return [c for c in g11.dat if 100 <= c < 400 and g11.dat[c][221] and ('SSFA', c) in g11.ssf and c not in MANAGERS_ETC]


def choose_base(g10, g11, cid, mv=None):
    if cid in BASE_OVERRIDE:
        return BASE_OVERRIDE[cid], 'override'
    r = g10.dat[cid]
    gender, wclass, scale = r[208], r[209], struct.unpack_from('<H', r, 28)[0]
    cands = [c for c in candidates_11(g11) if g11.dat[c][208] == gender]
    same = [c for c in cands if g11.dat[c][209] == wclass] or cands
    j = set(u16s(g10.pro[cid], 0x174 // 2)) - MARKERS
    generic = mv.generic(cid) if mv else False

    def score(c):
        shared = 0 if generic else len(j & set(u16s(g11.pro[c], 224)))
        return shared - abs(struct.unpack_from('<H', g11.dat[c], 28)[0] - scale) / 25.0
    best = max(same, key=score)
    return best, 'gender %d, weight class %d, scale %d vs %d, %s' % (
        gender, g11.dat[best][209], struct.unpack_from('<H', g11.dat[best], 28)[0], scale,
        'generic 2010 profile (no shared moves counted)' if generic else '%d move ids shared' % len(j & set(u16s(g11.pro[best], 224))))


def entrance_11(g10, g11, cid):
    """2011 entrance number for a 2010 character: same EVP name as his 2010 entrance, else as his name"""
    e11 = {}
    for n, s in g11.evp().items():
        if n < 600:
            e11.setdefault(norm(s), n)
    names = []
    if cid in g10.pro:
        e = g10.entrance_block(cid)[2]
        if e in g10.evp():
            names.append(g10.evp()[e])
    names += [g10.name(cid), g10.short(cid), g11.name(cid)]
    for s in names:
        if s and norm(s) in e11:
            return e11[norm(s)], s
    return None, None


def ra_candidates(g10, g11, cid):
    out = list(RA_ALIAS.get(cid, []))
    for s in (g10.name(cid), g11.name(cid), g10.short(cid)):
        if not s or s == '0':
            continue
        s = re.sub(r'[^A-Z0-9 \-]', '', s.upper())
        w = [x for x in s.split() if x not in ('THE', 'MR', 'JR')]
        for c in (''.join(s.split()), ''.join(w), w[-1] if w else '', w[0] if w else ''):
            if c and c not in out:
                out.append(c)
    return out


# --------------------------------------------------------------------------- build

def build(cid, out, o):
    pac10, pac11 = o['pac10'], o['pac11']
    g10, g11 = o['g10'], o['g11']
    snd = o['snd']
    work = o.get('work') or os.path.splitext(out)[0] + '.work'
    mod = os.path.join(work, 'mod')
    if os.path.exists(mod):
        shutil.rmtree(mod)
    os.makedirs(mod)
    log = []

    def p(*a):
        s = ' '.join(str(x) for x in a)
        print(s)
        log.append(s)
    own11 = cid in g11.pro and g11.named(cid) and ('SSFA', cid) in g11.ssf and g11.chpac(cid)
    in10 = cid in g10.dat and g10.named(cid)
    if not in10 and not own11:
        raise SystemExit('%d: neither a 2010 character nor a complete 2011 one' % cid)
    name = NAME_OVERRIDE.get(cid) or (g10.name(cid) if in10 else g11.name(cid).title())
    p('== %d %s (%s)' % (cid, name, '2010 character' if in10 else '2011 character, not in 2010'))
    mv = o.get('mv')
    if mv is None and in10 and not o.get('no_moves'):
        mv = o['mv'] = Moves(g10, g11, pac10, pac11, o['cache_work'])
    # base
    if o.get('base'):
        base, why = o['base'], 'option'
    elif own11:
        base, why = cid, '2011 ships this character complete (profile, model, renders): his own data'
    else:
        base, why = choose_base(g10, g11, cid, mv)
    if base not in candidates_11(g11) and base != cid:
        p('warning: base %d is not a playable 2011 superstar with renders' % base)
    p('base = %d %s (%s)' % (base, g11.name(base), why))
    man = collections.OrderedDict(type='superstar', id=re.sub(r'[^a-z0-9]+', '_', name.lower()).strip('_'),
                                  name=name[:31], short=(g10.short(cid) if in10 else name)[:31] or name[:31], base=base,
                                  author='SvR 2010 port' if in10 else 'SvR 2011 data', version='1.0',
                                  made_with='Port tools (SvR 2010)' if in10 else 'Port tools (SvR 2011 data)')
    # model
    use11 = o.get('model') == '11' or (o.get('model') is None and g11.chpac(cid))
    if use11:
        shutil.copyfile(g11.chpac(cid), os.path.join(mod, 'ch.pac'))
        p('model: 2011 %s (as shipped)' % os.path.basename(g11.chpac(cid)))
    else:
        kept = model_from_2010(pac10, cid, pac11, os.path.join(mod, 'ch.pac'))
        p('model: 2010 ch%d.pac converted (svr10_char: 2011 face animation, 256 bust portrait), entries %s' % (cid, kept))
    # renders
    if base != cid:
        src = g10 if (in10 and ('SSFB', cid) in g10.ssf) else g11
        for grp, fn in (('SSFB', 'render.dds'), ('SSFC', 'render_small.dds')):
            b = src.ssf.get((grp, cid))
            if b:
                open(os.path.join(mod, fn), 'wb').write(ch_tool.unpack(b))
        p('renders: %d DLC_HD SSFB/SSFC %04d' % (src.year, cid) if ('SSFB', cid) in src.ssf else 'renders: none (base\'s used)')
    # ratings, abilities
    r = g10.dat[cid] if in10 else None
    if r is not None and any(x != 50 for x in r[:7]):
        man['ratings'] = ','.join(str(max(0, x - 1)) for x in r[:7])
        ab = [x for x in r[230:238] if x in ABILITIES_2011][:5]
        if ab:
            man['abilities'] = ','.join(map(str, ab))
        p('ratings from 2010 DAT %s -> %s; abilities 2010 %s -> %s' % (list(r[:8]), man['ratings'], [x for x in r[230:238] if x], ab))
    elif r is not None:
        p('ratings: 2010 has only placeholders (all 50) -> the base\'s')
    else:
        # no real ratings anywhere: those of the 2011 superstar whose moves are most like his
        prof = set(u16s(g11.pro[cid], 224)) - MARKERS if cid in g11.pro else set()
        sim = max((c for c in candidates_11(g11) if c != cid and g11.dat[c][208] == g11.dat[cid][208]
                   and g11.dat[c][209] == g11.dat[cid][209] and any(x != 49 for x in g11.dat[c][:7])),
                  key=lambda c: len(prof & set(u16s(g11.pro[c], 224))))
        rr = g11.dat[sim]
        man['ratings'] = ','.join(str(x) for x in rr[:7])
        ab = [x for x in rr[230:238] if x][:5]
        if ab:
            man['abilities'] = ','.join(map(str, ab))
        p('ratings: none in either game (2011 holds placeholders) -> those of %d %s, the most similar moveset [guess]' % (sim, g11.name(sim)))
    # moves
    if in10 and not o.get('no_moves') and base != cid:
        if mv.generic(cid):
            p('moves: 2010 profile is the generic non-wrestler one -> base\'s moves kept')
        else:
            miss = mv.missing(cid)
            flags, man_moves = {}, None
            if miss:
                p('moves: %d 2010 moves have no 2011 motion -> porting %s' % (len(miss), miss))
                pdir = os.path.join(work, 'moveport')
                man_moves, ok = mv.port(miss, pdir, {}, o.get('verify'))
                flags = {w['id']: bytes.fromhex(w['flags_after']) for w in man_moves['misc']['waze']}
                for m in man_moves['moves']:
                    flags.setdefault(m, mv.w11[m][0] if m in mv.w11 else bytes(16))
                area, rows, extra = mv.assign(cid, base, flags)
                if extra:
                    p('moves: rebuilding with slot category bits %s' % extra)
                    man_moves, ok = mv.port(miss, pdir, extra, o.get('verify'))
                    flags = {w['id']: bytes.fromhex(w['flags_after']) for w in man_moves['misc']['waze']}
                    for m in man_moves['moves']:
                        flags.setdefault(m, mv.w11[m][0] if m in mv.w11 else bytes(16))
                if ok is not None:
                    p('move pack verify:', 'OK' if ok else 'FAILED')
                movepack.main(os.path.join(pdir, 'movepack'), os.path.join(mod, 'moves'))
            area, rows, extra = mv.assign(cid, base, flags)
            t = u16s(g11.pro[base], 224)
            lines = ['0x%03X=%d' % (k * 2, v) for k, v in enumerate(area) if v != t[k]]
            if lines:
                open(os.path.join(mod, 'moves.txt'), 'w', newline='\n').write('\n'.join(lines) + '\n')
                man['moves'] = 'moves.txt'
            with open(os.path.join(work, 'moves_table.tsv'), 'w', encoding='utf-8') as f:
                f.write('off2010\toff2011\tid\tname2010\tverdict\tid_in_slot\n')
                for row in rows:
                    f.write('0x%03x\t0x%03x\t%d\t%s\t%s\t%d\n' % row)
            kept = [r_ for r_ in rows if 'kept' in r_[4]]
            p('moves: %d slots over the base (%d ported moves used); base move kept in %d slots: %s' % (
                len(lines), sum(1 for r_ in rows if 'ported' in r_[4]), len(kept), [(hex(r_[1]), r_[2], r_[3]) for r_ in kept]))
    # entrance
    if o.get('entrance') or cid in ENTRANCE_OVERRIDE:
        man['entrance'] = o.get('entrance') or ENTRANCE_OVERRIDE[cid]
        p("entrance = %s '%s' (%s; the base has %d)" % (man['entrance'], g11.evp().get(man['entrance'], '?'),
                                                         'option' if o.get('entrance') else 'ENTRANCE_OVERRIDE', g11.entrance_block(base)[2]))
    elif in10:
        e, via = entrance_11(g10, g11, cid)
        be = g11.entrance_block(base)[2]
        how = '2011 ships his; matched on "%s"' % via
        if not e:
            # 2011 has none of his: a neutral Create-An-Entrance one rather than the base's own
            # (whose poses and pyro belong to the base)
            e = 483 if g10.dat[cid][208] else 522
            how = 'no entrance of his in 2011 -> generic'
        if e and e != be:
            man['entrance'] = e
            p('entrance = %d "%s" (%s)' % (e, g11.evp()[e], how))
        else:
            p('entrance: 2011 has none of his -> base\'s %d "%s"' % (be, g11.evp().get(be, '?')))
    # theme / movie
    if in10:
        music, movie = g10.entrance_block(cid)[:2]
        th = None
        for m in (music, cid):
            if m != 255 and theme10(pac10, m, os.path.join(mod, 'theme.mp3')):
                th = m
                break
        if th is not None:
            man['song'] = 'theme.mp3'
            p('theme: 2010 music1.yaf %d (music id %d) -> theme.mp3, %.1f LUFS -> %+.1f dB -> %.1f LUFS' % ((th * 100, th) + theme10.last))
        else:
            p('theme: none in 2010 (music id %d)' % music)
        for m in (movie, cid):
            if m != 255 and g10.titantron(m):
                shutil.copyfile(g10.titantron(m), os.path.join(mod, 'movie.bik'))
                man['movie'] = 'movie.bik'
                p('titantron: 2010 movies/titantron/%03d.bik' % m)
                break
        else:
            p('titantron: none in 2010 (movie id %d)' % movie)
    else:
        mu, mo = g11.entrance_block(cid)[:2]
        p('theme: 2011 Music.pck Play_MUS_%04d_0_0: %s; titantron %03d.bik: %s' % (
            mu, snd.theme(mu) or 'no event', mo, 'yes' if g11.titantron(mo) else 'none'))
    # announcer / commentary
    nm, clips = snd.announcer(ra_candidates(g10, g11, cid))
    if nm:
        man['announcer'] = nm
        p('announcer = %s (2011 RA.pck clips %s)' % (nm, ' '.join(clips)))
    else:
        p('announcer: no 2011 recordings of the name')
    if snd.commentary(cid) and base == cid:
        p('commentary: 2011 Comm_%04d (the base\'s own bank)' % cid)
    else:
        c = o.get('call') if o.get('call') is not None else CALL.get(cid, 69 if (g10.dat.get(cid) or g11.dat[cid])[208] else 77)
        man['call'] = c
        p('commentary: call=%d (nickname bank; 2011 has no Comm_%04d)' % (c, cid))
    if o.get('height'):
        h = round(min(1.25, max(0.80, float(o['height']))), 2)
        if h != 1.0:
            man['height'] = '%.2f' % h
            p('height = %.2f' % h)
    # manifest + zip
    order = ['type', 'id', 'name', 'short', 'base', 'author', 'made_with', 'version', 'ratings', 'abilities', 'moves', 'entrance',
             'announcer', 'call', 'height', 'song', 'movie']
    txt = ''.join('%s=%s\n' % (k, man[k]) for k in order if k in man)
    open(os.path.join(mod, 'manifest.txt'), 'w', newline='\n').write(txt)
    if os.path.exists(os.path.join(mod, 'ch.pac')):   # Superstar Threads parts from the model's masks
        try:
            import threads_parts
            parts = threads_parts.build(mod, os.path.dirname(os.path.normpath(pac11)))
            if parts:
                threads_parts.write(mod, parts)
            p('Superstar Threads: %s' % ('%d part(s)' % sum(len(a['parts']) for a in parts) if parts else 'not listed (no masks)'))
        except Exception as e:   # noqa: BLE001 - never fails the port
            p('Superstar Threads parts not made (threads_parts: %s)' % e)
    tmp = out + '.%d.tmp' % os.getpid()
    with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED) as z:
        for root, dirs, files in os.walk(mod):
            for f in sorted(files):
                fp = os.path.join(root, f)
                z.write(fp, os.path.relpath(fp, mod).replace('\\', '/'))
    import time
    for attempt in range(30):   # a virus scanner (or a reader) may hold the old file for a moment
        try:
            os.replace(tmp, out)
            break
        except PermissionError:
            time.sleep(2)
    else:
        raise SystemExit('could not replace %s (in use); the new mod is %s' % (out, tmp))
    p('wrote %s (%d bytes)' % (out, os.path.getsize(out)))
    p(txt)
    open(os.path.join(work, 'build.log'), 'w', encoding='utf-8').write('\n'.join(log) + '\n')
    return man


# --------------------------------------------------------------------------- survey

def survey(o, out):
    g10, g11, snd = o['g10'], o['g11'], o['snd']
    rows = []
    ids = [c for c in sorted(g10.dat) if 100 <= c < 400 and g10.named(c) and not g10.name(c).upper().startswith(('MOTION', 'MOVES MAN'))
           and c not in (255, 256, 257)]
    ids = [c for c in ids if not (g11.named(c) and g11.dat[c][221] == g10.dat[c][221] and g11.chpac(c))]
    ids.append(274)
    t10 = yaf_table(os.path.join(os.path.dirname(o['pac10']), 'sound', 'music1.yaf'))
    for c in ids:
        in10 = g10.named(c)
        r10 = g10.dat.get(c)
        name = g10.name(c) if in10 else g11.name(c).title()
        st11 = []
        if not g11.named(c):
            st11.append('blank record')
        elif not g11.dat[c][221]:
            st11.append('record "%s", not selectable' % g11.name(c))
        st11.append('ch.pac' if g11.chpac(c) else 'no ch.pac')
        st11.append('renders' if ('SSFA', c) in g11.ssf else 'no renders')
        e, via = entrance_11(g10, g11, c) if in10 else (None, None)
        if c == 274:
            e = '%d (profile) / 549 "Hurricane"' % g11.entrance_block(274)[2]
        nm, clips = snd.announcer(ra_candidates(g10, g11, c))
        music10, movie10 = (g10.entrance_block(c)[:2] if in10 else (None, None))
        mus11 = snd.theme(c)
        rows.append(dict(
            id=c, name=name, sel10=(r10[221] if in10 else '-'), gender='diva' if (r10 if in10 else g11.dat[c])[208] else 'male',
            status11='; '.join(st11),
            entrance11=e or '-', announcer11=('%s (%s)' % (nm, ','.join(x for x in clips if 'SSN_0' in x)) if nm else '-'),
            comm11=('Comm_%04d' % c) if snd.commentary(c) else '-',
            theme11=mus11 or '-', tron11=('%03d.bik' % c) if g11.titantron(c) else '-',
            model10=('ch%d.pac' % c) if g10.chpac(c) else '-',
            renders10='SSFA/B/C' if ('SSFB', c) in g10.ssf else '-',
            theme10=('music1 %d' % (music10 * 100)) if in10 and music10 * 100 in t10 else ('music1 %d' % (c * 100) if in10 and c * 100 in t10 else '-'),
            tron10=('%03d.bik' % movie10) if in10 and movie10 != 255 and g10.titantron(movie10) else ('%03d.bik' % c if in10 and g10.titantron(c) else '-'),
            moves_missing=(len(o['mv'].missing(c)) if in10 and o.get('mv') and not o['mv'].generic(c) else ('generic' if in10 and o.get('mv') else '-')),
        ))
        print(rows[-1])
    keys = list(rows[0].keys())
    with open(out, 'w', encoding='utf-8') as f:
        f.write('\t'.join(keys) + '\n')
        for r in rows:
            f.write('\t'.join(str(r[k]) for k in keys) + '\n')
    return rows


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    o = dict(pac11=PAC11)
    args = []
    i = 1
    while i < len(argv):
        a = argv[i]
        if a in ('--pac10', '--pac11', '--base', '--model', '--entrance', '--call', '--work', '--out', '--height'):
            o[a[2:]] = argv[i + 1]
            i += 2
            continue
        if a in ('--no-moves', '--verify'):
            o[a[2:].replace('-', '_')] = True
        else:
            args.append(a)
        i += 1
    if 'pac10' not in o:
        raise SystemExit('svr10_superstar: --pac10 <the SvR 2010 pac folder> is needed')
    for k in ('base', 'entrance', 'call'):
        if k in o:
            o[k] = int(o[k])
    o['g10'], o['g11'] = Game(o['pac10'], 10), Game(o['pac11'], 11)
    o['snd'] = Sound11(o['pac11'])
    if args[0] == 'survey':
        o['cache_work'] = o.get('work') or os.path.join(HERE, 'work')
        o['mv'] = Moves(o['g10'], o['g11'], o['pac10'], o['pac11'], o['cache_work'])
        survey(o, o.get('out') or os.path.join(HERE, 'roster.tsv'))
        return
    ids = [int(x) for x in args[0].split(',')]
    outs = args[1:]
    o['cache_work'] = os.path.join(os.path.dirname(os.path.abspath(outs[0] if len(outs) == 1 else outs[0])), 'work')
    if len(ids) > 1:   # several: <ids> <out folder>
        os.makedirs(outs[0], exist_ok=True)
        res = {}
        for c in ids:
            nm = (o['g10'].name(c) if o['g10'].named(c) else o['g11'].name(c)).lower()
            fn = os.path.join(outs[0], re.sub(r'[^a-z0-9]+', '_', nm).strip('_') + '.svrmod')
            try:
                build(c, fn, dict(o, work=None))
                res[c] = 'ok ' + fn
            except (Exception, SystemExit) as e:   # keep going with the others
                import traceback
                traceback.print_exc()
                res[c] = 'FAILED: %s' % e
        for c, v in res.items():
            print(c, v)
    else:
        build(ids[0], outs[0], o)


if __name__ == '__main__':
    main(sys.argv)
