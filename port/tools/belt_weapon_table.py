"""Builds tools/data/belt_weapon_textures.json: SvR 2011's own title belt and
weapon textures with their texture-pack hashes (developer tool; needs xxhash).

    py tools/belt_weapon_table.py <SvR 2011 game folder or ISO>

belt_port.py / weapon_port.py read the table, so players need no xxhash. A
texture's hash16 is what texture packs match on (native/texture_packs.h):
XXH3-64 of its top level's blocks as stored in the pac (the PC DDS payload).

Belts: pac/evt/evtobj.pac SOBJ (SOBJ/LIST: 64-byte records, u16 id + name;
"<title> - Stretched / Folded / Rolled"), and the belt picked up in a match,
bg/bgEtc.pac STG/WPON/0x000E (14 packs; each one's model is named by its
SOBJ id). Weapons: bgEtc.pac STG/WPON packs (model JBOY 0x0, texture set
0xA), named from the weapon settings text (0x9C40: model number = pack id x 10).
"""
import json
import os
import re
import struct
import sys

import xxhash

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import discfs  # noqa: E402
import svrfmt  # noqa: E402

TITLES = [  # key, the LIST name's title part, the name shown
    ('IC', 'IC Title', 'Intercontinental Championship'),
    ('US', 'U.S. Title', 'United States Championship'),
    ('WWE', 'WWE Title', 'WWE Championship'),
    ('HARDCORE', 'Hardcore Title', 'Hardcore Championship'),
    ('WOMENS', "Women's Title", "Women's Championship"),
    ('WWE_TAG', 'WWE Tag Team Title', 'WWE Tag Team Championship'),
    ('CRUISER', 'Cruiserweight Title', 'Cruiserweight Championship'),
    ('WHC', 'World Heavyweight Title', 'World Heavyweight Championship'),
    ('WORLD_TAG', 'Wolrd Tag Team Title', 'World Tag Team Championship'),
    ('ECW', 'ECW Title', 'ECW Championship'),
    ('DIVAS', 'Divas Title', 'Divas Championship'),
    ('MILLION', 'Million dollar Title', 'Million Dollar Championship'),
]

# weapon settings text comments (Japanese) -> English names, by model number
WEAPON_NAMES = {
    10: 'Barbed wire bat', 16: 'Replica belt', 20: 'Raised announce table', 30: 'Ring bell', 40: 'Steel chair',
    50: 'Table', 52: 'Table (broken half)', 60: 'Announce table', 70: 'Announce table lid', 80: 'Announce table cable',
    90: 'Shovel', 110: 'Ladder', 140: 'Title belt', 150: 'Replica title belt', 320: 'Steel steps',
    330: 'Steel steps (top)', 331: 'Steel steps (top)', 340: 'Steel steps (bottom)', 400: 'Breakable sign',
    410: 'Breakable sign', 420: 'Breakable sign', 430: 'Breakable sign', 440: 'Breakable sign', 450: 'Breakable sign',
    600: 'Crutch', 610: 'Cane', 620: 'Umbrella', 630: 'Trash can', 650: 'Fire extinguisher', 660: 'Speaker',
    670: 'Monitor', 680: 'Beer bottle (crowd)', 700: 'Fluorescent tube', 730: 'Sign', 740: 'Sign', 750: 'Sign',
    760: 'Sign', 770: 'Sign', 780: 'Sign', 790: 'Table debris', 800: 'Table debris', 810: 'Table debris',
    820: 'Table debris', 830: 'Popcorn', 840: 'Tire', 850: 'Bumper', 860: 'Broken TV', 1000: 'Announce table cable',
    1020: "Triple H's sledgehammer", 1100: 'Beer bottle (bar)', 1110: 'Baseball bat', 1120: 'Pool cue',
    1130: 'TV (black)', 1200: 'Tire iron', 1210: 'Horn', 1300: 'TV (white)', 1310: 'TV camera', 1400: 'Pillow',
}


un = svrfmt.unbpe


def texture_set(raw):
    if len(raw) < 48:
        return []
    n = struct.unpack_from('<I', raw, 0)[0]
    if not (0 < n < 300) or 16 + 32 * n > len(raw) or raw[32:35] != b'dds':
        return []
    out = []
    for i in range(n):
        r = raw[16 + 32 * i:48 + 32 * i]
        size, off = struct.unpack_from('<II', r, 20)
        out.append((r[:16].split(b'\0')[0].decode('latin-1'), raw[off:off + size]))
    return out


def top_level(dds):
    """(width, height, format, mips, top level bytes) of a PC DDS."""
    h, w = struct.unpack_from('<II', dds, 12)
    mips = max(1, struct.unpack_from('<I', dds, 28)[0])
    four = dds[84:88]
    if four in (b'DXT1', b'DXT3', b'DXT5'):
        block = 8 if four == b'DXT1' else 16
        size = max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * block
        fmt = four.decode()
    else:
        bits = struct.unpack_from('<I', dds, 88)[0] or 32
        size = w * h * bits // 8
        fmt = 'A8R8G8B8' if bits == 32 else 'RGB%d' % bits
    return w, h, fmt, mips, dds[128:128 + size]


def entry(owner, pac, where, name, dds):
    w, h, fmt, mips, top = top_level(dds)
    return {'name': name, 'owner': owner, 'width': w, 'height': h, 'format': fmt, 'mips': mips,
            'hash16': xxhash.xxh3_64_hexdigest(top), 'pac': pac, 'where': where}


def children(blob):
    x = un(blob)
    return svrfmt.pach_read(x) if x[:4] == b'PACH' else []


def belts(disc):
    data = disc.read('pac/evt/evtobj.pac')
    _, groups, _ = svrfmt.epac_read(data)
    entries = {}
    names = {}
    for _, ents in groups:
        for nm, blob in ents:
            key = nm.split(b'\0')[0].decode('latin-1')
            if key == 'LIST':
                x = un(blob)
                for o in range(0, len(x) - 63, 64):
                    i = struct.unpack_from('<H', x, o)[0]
                    names[i] = x[o + 2:o + 64].split(b'\0')[0].decode('cp932', 'replace')
            else:
                entries[key] = blob
    out = []
    for key, part, shown in TITLES:
        ids = sorted(i for i, n in names.items() if n.startswith(part + ' -') or n.startswith(part + '  -'))
        seen, texs = set(), []
        for i in ids:
            blob = entries.get('%04d' % i)
            if blob is None:
                continue
            for cid, child in children(blob):
                for tname, dds in texture_set(un(child)):
                    if tname.lower() in seen or tname == 'ar_ref':
                        continue
                    seen.add(tname.lower())
                    texs.append(entry('evtobj', 'pac/evt/evtobj.pac', 'SOBJ/%04d/%x' % (i, cid), tname, dds))
        out.append({'key': key, 'name': shown, 'sobj': ids, 'textures': texs})
    return out


def wpon(disc):
    data = disc.read('pac/bg/bgEtc.pac')
    _, groups, _ = svrfmt.epac_read(data)
    for typ, ents in groups:
        if typ.strip() != b'STG':
            continue
        for nm, blob in ents:
            if nm.split(b'\0')[0] == b'WPON':
                return svrfmt.pach_read(blob)
    return []


def weapons(disc, belt_list):
    packs = wpon(disc)
    table = {}
    for i, x in packs:
        if i == 0x9C40:
            for line in un(x).decode('cp932', 'replace').splitlines():
                m = re.match(r'\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*([\d.]+)', line)
                if m:
                    v = [int(m.group(k)) for k in range(1, 7)]
                    table[v[0]] = {'type': v[1], 'se_hit': v[2], 'se_drop': v[3], 'hp': v[4], 'weight': v[5],
                                   'damage': float(m.group(7))}
    out = []
    belt_of = {}  # SOBJ id -> title key (the WPON 0xE belt packs)
    for b in belt_list:
        for i in b['sobj']:
            belt_of[i] = b['key']
    for pid, blob in packs:
        if pid >= 0x9C40:
            continue
        if pid == 0xE:  # the match belts: their textures go with their titles
            for sub, sb in children(blob):
                model, texs = None, []
                for cid, child in children(sb):
                    c = un(child)
                    if c[:4] == b'JBOY':
                        p = struct.unpack_from('>I', c, 0x30)[0] + 8
                        model = c[p:p + 16].split(b'\0')[0].decode('latin-1')
                    for tname, dds in texture_set(c):
                        texs.append(entry('bgEtc', 'pac/bg/bgEtc.pac', 'STG/WPON/e/%x/%x' % (sub, cid), tname, dds))
                sobj = int(model[:4]) if model and model[:4].isdigit() else None
                key = belt_of.get(sobj)
                for b in belt_list:
                    if b['key'] == key:
                        names = {t['name'].lower() for t in b['textures']}
                        b['textures'] += [t for t in texs if t['name'].lower() not in names]
            continue
        texs = []
        for cid, child in children(blob):
            for tname, dds in texture_set(un(child)):
                if tname.lower() not in {t['name'].lower() for t in texs}:
                    texs.append(entry('bgEtc', 'pac/bg/bgEtc.pac', 'STG/WPON/%x/%x' % (pid, cid), tname, dds))
        if not texs:
            continue
        model = pid * 10
        info = table.get(model, {})
        name = WEAPON_NAMES.get(model) or 'Item %d (%s)' % (model, texs[0]['name'])
        out.append({'key': '%d' % model, 'pack': '0x%x' % pid, 'name': name, 'settings': info, 'textures': texs})
    return out


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    with discfs.open(sys.argv[1]) as disc:
        b = belts(disc)
        w = weapons(disc, b)
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data', 'belt_weapon_textures.json')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        json.dump({'game': 'svr2011', 'belts': b, 'weapons': w}, f, indent=1, ensure_ascii=False)
    print('%s: %d titles (%d textures), %d weapons (%d textures)' % (
        path, len(b), sum(len(x['textures']) for x in b), len(w), sum(len(x['textures']) for x in w)))


if __name__ == '__main__':
    main()
