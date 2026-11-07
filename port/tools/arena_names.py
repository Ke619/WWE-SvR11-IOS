"""Arena names and thumbnails from a WWE '12 - 2K17 disc, for arena_port.py's list / scan.

Names: the game's own string table. Every one of these games keeps its arena names as one run of
string ids, one per stage number: name of bgNN.pac = string (base + NN). The base is found from the
run itself - the two names 'BLACK ROOM' / 'WHITE ROOM' are always stages 76 / 77 (WWE '12 - 2K14:
base 40002, 2K15 - 2K17: 18000) - so the special stages (60+) get names too (GM ROOM, BACKSTAGE ...).
  string tables: WWE '12 / '13 pac/string.pac SDB '0000' (16-byte records {offset, length, id, 0});
  2K14 - 2K16 pac/string.pac SDB '5000' child 0 (12-byte records {offset, length, id}; 2K16
  xcompressed); 2K17 pac/string_360.pac, xcompressed HSPC, its first SHDC with the run (English).
Thumbnails: the match intro's stage backdrop, pac/menu/MatchHD.pac MLYE/MB<NN> (the texture 'bg',
1024 x 512), the same picture the loading screen shows. 2K17 keeps MatchHD.pac as an HSPC with
hashed entry names; its texture bundles (ZLIB leaves of its SHDCs) keep their names, and one holds the arena
select pictures Ar<NN> (256 x 128, NN = the stage number): those are its thumbnails.
"""
import json
import os
import re
import struct
import zlib

import discfs  # noqa: F401 (callers pass a discfs disc)
import svrfmt

XCOMPRESS = b'\x0f\xf5\x12\xed'
CACHE_VERSION = 3  # (2: thumbs/bgNN_full.png, the backdrop at its own size, beside each thumb; 3: 2K17's Ar<NN>)


def _unz(b):
    if b[:4] == b'BPE ':
        return svrfmt.bpe_decode(b)
    if b[:4] == b'ZLIB':
        cs, us = struct.unpack_from('<II', b, 8)
        return b[16:16 + us] if cs == us else zlib.decompress(b[16:16 + cs])
    return b


def _xdec(raw, svrmod):
    if raw[:4] != XCOMPRESS:
        return raw
    import wwe13_entrance as WE
    return WE.xdec(raw, WE.find_svrmod(svrmod))


def _table(u):
    """{id: text} from a string table ({u32 0, u32 count, records {offset, length, id[, 0]}})"""
    if len(u) < 8:
        return {}
    n = struct.unpack_from('<I', u, 4)[0]
    for rec in (16, 12):
        if not n or 8 + rec * n > len(u):
            continue
        out = {}
        for k in range(n):
            off, ln, sid = struct.unpack_from('<3I', u, 8 + rec * k)
            if off + ln > len(u):
                out = None
                break
            out[sid] = u[off:off + ln].split(b'\0')[0].decode('utf-8', 'replace').strip()
        if out:
            return out
    return {}


def _base(strings):
    for k, v in strings.items():
        if v.upper() == 'BLACK ROOM' and strings.get(k + 1, '').upper() == 'WHITE ROOM':
            return k - 76
    return None


def _shdc_leaves(b):
    n = struct.unpack_from('<I', b, 0x38)[0]
    t = struct.unpack_from('<I', b, 0x1C)[0]
    return [b[struct.unpack_from('<I', b, t + 16 * i + 4)[0]:][:struct.unpack_from('<I', b, t + 16 * i + 8)[0]]
            for i in range(n) if t + 16 * i + 12 <= len(b)]


def _hspc_blocks(d):
    """the BPE / SHDC blocks of an HSPC file, decoded"""
    out = []
    for m in re.finditer(rb'BPE \x00\x01\x00\x00|SHDC', d):
        b = d[m.start():]
        if b[:4] == b'BPE ':
            try:
                out.append(svrfmt.bpe_decode(b))
            except Exception:  # noqa: BLE001 - a false match
                pass
        else:
            out.append(b)
    return out


def strings(disc, svrmod=None):
    """the English string table of a WWE '12 - 2K17 disc that holds the arena names -> (strings, base); a DLC package's
    string table (dlc_src.SourceView) fills the numbers the disc's has no name for: its arenas"""
    t, base = _strings(disc, ('pac/string.pac', 'pac/string_360.pac'), svrmod)
    if base is not None:
        for pk in getattr(getattr(disc, 'dlc', None), 'packages', []):
            for rel in pk.matching(r'^pac/string[^/]*\.pac$'):
                t2, base2 = _strings(pk, (rel,), svrmod)
                if base2 is not None:
                    for n in range(100):
                        v = t2.get(base2 + n, '')
                        if v.strip() and v.strip().lower() not in ('(null)', '-') and not t.get(base + n, '').strip():
                            t[base + n] = v
    return t, base


def _strings(disc, rels, svrmod=None):
    for rel in rels:
        if hasattr(disc, 'files') and rel not in disc.files:
            continue
        if not hasattr(disc, 'files') and not disc.exists(rel):
            continue
        raw = _xdec(disc.read(rel), svrmod)
        tables = []
        if raw[:4] in (b'EPAC', b'EPK8'):
            _, g, _ = svrfmt.epac_read(raw)
            es = {n.strip(b'\0 '): b for t, ents in g for n, b in ents if t.startswith(b'SDB')}
            for key in (b'0000', b'5000') + tuple(k for k in es if k not in (b'0000', b'5000')):
                if key not in es:
                    continue
                u = _unz(es[key])
                kids = svrfmt.pach_read(u) if u[:4] == b'PACH' else [(0, u)]
                merged = {}
                for _, x in kids:
                    merged.update(_table(_unz(x)))
                tables.append(merged)
        elif raw[:4] == b'HSPC':
            for b in _hspc_blocks(raw):
                if b[:4] == b'SHDC':
                    leaves = _shdc_leaves(b)
                    if leaves:
                        tables.append(_table(_unz(leaves[0])))
        for t in tables:
            base = _base(t)
            if base is not None:
                return t, base
    return {}, None


def arena_name(strings_, base, number):
    """the game's name of stage <number> (None: no name / a '(null)' placeholder)"""
    if base is None:
        return None
    v = strings_.get(base + number, '').strip()
    return None if not v or v.lower() in ('(null)', '-') else ' '.join(v.split())


GENERIC = ('dummy', 'grad', 'matchup_bg', 'highlight', 'tonight_')


def _backdrop(entry):
    """the stage backdrop among an arena picture entry's textures: named bg / <key>_bg / bg_<key>, else a
    1024 x 512 picture (WWE '12: RAW01), else the biggest one that isn't a shared overlay - or None"""
    u = _unz(entry)
    kids = svrfmt.pach_read(u) if u[:4] == b'PACH' else [(0, u)]
    texs = []
    for _, x in kids:
        x = _unz(x)
        n = struct.unpack_from('<I', x, 0)[0] if len(x) >= 48 else 0
        if not (0 < n < 600) or x[32:35] != b'dds':
            continue
        for i in range(n):
            r = x[16 + 32 * i:48 + 32 * i]
            sz, off = struct.unpack_from('<II', r, 20)
            dds = x[off:off + sz]
            if dds[:4] != b'DDS ' or len(dds) < 128:
                continue
            h, w = struct.unpack_from('<II', dds, 12)
            texs.append((r[:16].split(b'\0')[0].decode('latin1').lower(), w, h, dds))
    named = [t for t in texs if re.search(r'(^|_)bg($|_)', t[0]) and t[0] != 'matchup_bg']
    wide = [t for t in texs if (t[1], t[2]) == (1024, 512) and not t[0].startswith(GENERIC)]
    rest = sorted((t for t in texs if not t[0].startswith(GENERIC)), key=lambda t: -t[1] * t[2])
    for group in (named, wide, rest):
        if group:
            return group[0][3]
    return None


def _hspc_textures(raw, pattern):
    """{name: dds} of an HSPC menu pac's texture bundles (2K17) whose names match the regex pattern"""
    out = {}
    for b in _hspc_blocks(raw):
        if b[:4] != b'SHDC':
            continue
        for leaf in _shdc_leaves(b):
            try:
                x = _unz(leaf)
            except Exception:  # noqa: BLE001 - not a bundle
                continue
            n = struct.unpack_from('<I', x, 0)[0] if len(x) >= 48 else 0
            if not (0 < n < 600) or x[32:35] != b'dds':
                continue
            for i in range(n):
                r = x[16 + 32 * i:48 + 32 * i]
                name = r[:16].split(b'\0')[0].decode('latin1')
                if not re.fullmatch(pattern, name):
                    continue
                sz, off = struct.unpack_from('<II', r, 20)
                if x[off:off + 4] == b'DDS ':
                    out.setdefault(name, x[off:off + sz])
    return out


def thumbnails(disc, numbers, out_dir, svrmod=None, size=(320, 160), progress=None):
    """{number: png path} - each stage's backdrop: the match intro's (MatchHD.pac MLYE/MB<NN>), else the
    loading screen's (LoadHD.pac LOAD/90<NN>, WWE '12 also 70<NN>) - the same pictures. Beside each thumb
    (bgNN.png, size) the backdrop at full resolution, 2:1 as the thumb: bgNN_full.png (full_picture(); a mod's
    loading picture)"""
    import io
    from PIL import Image
    es = {}
    select = {}   # (2K17: the arena select pictures Ar<NN>)
    for rel in ('pac/menu/MatchHD.pac', 'pac/menu/LoadHD.pac'):
        if not disc.exists(rel):
            continue
        raw = _xdec(disc.read(rel), svrmod)
        if raw[:4] == b'HSPC':
            if not select:
                select = _hspc_textures(raw, r'Ar\d\d')
            continue
        if raw[:4] not in (b'EPAC', b'EPK8'):
            continue
        _, g, _ = svrfmt.epac_read(raw)
        for t, ents in g:
            for n, b in ents:
                es.setdefault(n.strip(b'\0 ').decode('latin1'), b)
    os.makedirs(out_dir, exist_ok=True)
    out = {}
    for k, num in enumerate(numbers):
        if progress:
            progress(60 + 35 * k // max(1, len(numbers)), 'arena pictures %d / %d' % (k + 1, len(numbers)))
        dds = None
        for key in ('MB%02d' % num, '90%02d' % num, '70%02d' % num):
            if key in es and num < 100:
                dds = _backdrop(es[key])
                if dds:
                    break
        if not dds:
            dds = select.get('Ar%02d' % num)
        if not dds:
            continue
        try:
            full = Image.open(io.BytesIO(dds)).convert('RGB')
            im = full.resize(size, Image.LANCZOS)
        except Exception:  # noqa: BLE001 - a picture the decoder can't read: no thumbnail
            continue
        path = os.path.join(out_dir, 'bg%02d.png' % num)
        im.save(path)
        w = max(full.width, 2 * full.height)  # (2:1 as the thumb - WWE '13's are 512 x 512 squeezed: 1024 x 512)
        (full if full.size == (w, w // 2) else full.resize((w, w // 2), Image.LANCZOS)).save(full_picture(path))
        out[num] = path
    return out


def full_picture(thumb):
    """The full-size backdrop saved beside a thumb (bgNN.png -> bgNN_full.png)"""
    return os.path.splitext(thumb)[0] + '_full.png'


def cache_dir(root, game, src):
    """<root>/<game>_<a short key of the source path, size and time>"""
    st = os.stat(src)
    key = '%08x' % (zlib.crc32(('%s|%d|%d' % (os.path.abspath(src).lower(), st.st_size, int(st.st_mtime))).encode()) & 0xffffffff)
    return os.path.join(root, '%s_%s' % (game, key))


def load_cache(path):
    try:
        with open(os.path.join(path, 'arenas.json'), encoding='utf-8') as f:
            c = json.load(f)
        return c if c.get('version') == CACHE_VERSION else None
    except (OSError, ValueError):
        return None


def save_cache(path, data):
    os.makedirs(path, exist_ok=True)
    data = dict(data, version=CACHE_VERSION)
    with open(os.path.join(path, 'arenas.json'), 'w', encoding='utf-8') as f:
        json.dump(data, f, ensure_ascii=False, indent=1)
