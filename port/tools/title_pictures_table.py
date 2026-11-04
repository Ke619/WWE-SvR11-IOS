"""Builds tools/data/title_pictures.json: the menus' title pictures (belt_<title> in menuHD.pac / menuSD.pac
MEN?/CMSM and CMSI, every language group) with their texture-pack hashes (developer tool; needs xxhash).

    py tools/title_pictures_table.py <SvR 2011 game folder or ISO>

title_port.py reads the table, so players need no xxhash (as belt_weapon_table.py for the belts).
"""
import json
import os
import struct
import sys

import xxhash

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import discfs  # noqa: E402
import svrfmt  # noqa: E402
from belt_weapon_table import top_level  # noqa: E402

# title key -> the picture's texture name in the menu bundles
PICTURES = {'WWE': 'belt_WWE', 'WHC': 'belt_WorldHeavy', 'WWE_TAG': 'belt_WWETag', 'WORLD_TAG': 'belt_WorldTag',
            'IC': 'belt_Interconti', 'US': 'belt_US', 'CRUISER': 'belt_Cruiser', 'WOMENS': 'belt_Womens',
            'HARDCORE': 'belt_Hardcore', 'ECW': 'belt_ECW', 'MILLION': 'belt_million', 'DIVAS': 'belt_Divas', 'WCW': 'belt_WCW'}

# The title's art on the championship screens (Championship Scramble's title list: the picture beside the holder):
# BG_<title> in menuHD / menuSD, outside the texture bundles this reads - hashes from a texture dump's name index
# (UserData\cache\texture_names.txt, 2.1.10). The tag titles have none.
ART = {'IC': [('menuHD', 'BG_IC', 'b34ce61d31865f30'), ('menuSD', 'BG_IC', 'c35503e3bf96890c')],
       'US': [('menuHD', 'BG_US', '1ef9e3192ab48caf'), ('menuSD', 'BG_US', 'b15124f114fceb9f')],
       'WWE': [('menuHD', 'BG_wwe', '52bd9dcc80684d9d'), ('menuSD', 'BG_wwe', 'b412066623c7f4ad')],
       'WHC': [('menuHD', 'BG_WHW', '354deb8be292674c'), ('menuSD', 'BG_WHW', '2b4e319c5dab2b22')],
       'ECW': [('menuHD', 'BG_ECW', '625bd6c57c58e986'), ('menuSD', 'BG_ECW', '05aa34ccdfdb0480')],
       'DIVAS': [('menuHD', 'BG_diva', 'ed0cc42968d28d8d'), ('menuSD', 'BG_diva', 'd193ce9b9f3f8748')],
       'WOMENS': [('menuHD', 'BG_woman', 'cb78cd831af889f3'), ('menuSD', 'BG_woman', '2ff0ac94b41de2f2')],
       'HARDCORE': [('menuHD', 'BG_Hardcore', 'b9ae50f9de6bee64'), ('menuSD', 'BG_Hardcore', '6fe1acd0c1137105')],
       'CRUISER': [('menuHD', 'BG_Cruiser', 'f0791025db128d69'), ('menuSD', 'BG_Cruiser', 'ab27159816deb020')],
       'MILLION': [('menuHD', 'BG_millon', '8ef86235257cad5b'), ('menuSD', 'BG_millon', '060194029fc26726')],
       'WCW': [('menuHD', 'BG_WCW', 'b97357a3d10f074f'), ('menuSD', 'BG_WCW', '5665a7c091aa1cd1')]}

def un(b):
    return svrfmt.bpe_decode(b) if b[:4] == b'BPE ' else b


def main():
    out = {k: [] for k in PICTURES}
    with discfs.open(sys.argv[1]) as disc:
        for pac in ('pac/menu/menuHD.pac', 'pac/menu/menuSD.pac'):
            _, groups, _ = svrfmt.epac_read(disc.read(pac))
            for grp, ents in groups:
                for name, blob in ents:
                    if name not in (b'CMSM', b'CMSI'):
                        continue
                    for cid, cb in svrfmt.pach_read(un(blob)):
                        raw = un(cb)
                        n = struct.unpack_from('<I', raw, 0)[0] if len(raw) >= 4 else 0
                        for k in range(n if 16 + 32 * n <= len(raw) else 0):
                            rec = raw[16 + 32 * k:48 + 32 * k]
                            tname = rec[:16].split(b'\0')[0].decode('latin-1')
                            key = next((t for t, p in PICTURES.items() if p == tname), None)
                            if not key:
                                continue
                            size, off = struct.unpack_from('<II', rec, 20)
                            w, h, fmt, mips, top = top_level(raw[off:off + size])
                            h16 = xxhash.xxh3_64_hexdigest(top)
                            owner = os.path.splitext(os.path.basename(pac))[0]
                            if not any(e['hash16'] == h16 for e in out[key]):
                                out[key].append({'name': tname, 'owner': owner, 'width': w, 'height': h, 'format': fmt,
                                                 'hash16': h16, 'pac': pac,
                                                 'where': '%s/%s/%x' % (grp.decode().strip(), name.decode(), cid)})
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data', 'title_pictures.json')
    with open(path, 'w', newline='\n') as f:
        json.dump({'pictures': out, 'art': {k: [{'owner': o, 'name': n, 'hash16': h} for o, n, h in v] for k, v in ART.items()}}, f, indent=1)
    print(path, {k: len(v) for k, v in out.items()})


if __name__ == '__main__':
    main()
