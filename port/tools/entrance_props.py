"""Entrance props a converted entrance brings along: a source game's own vehicle as a new pac\\evt\\evtobj.pac
SOBJ entry, added by the move pack (`pacentry evt\\evtobj.pac SOBJ <id> props/<id>.bin`; the game copies
evtobj.pac into Mods\\PacOverlay with it, never writes its own) with `propfallback <id> <game prop>`: when that
copy can't be made, the entrance uses the game's prop instead (move_packs.cpp).

Ids: one fixed id per source model (two packs adding the same bike add the same entry; the first one wins).
- WWE 2K14 2540 (the Undertaker's black bike, 210 / 409) = 2011's bike 2540 (the same mesh and nodes) with 2K14's
  textures (bike01 black with flames, a blank number plate): 2011's entry with its texture bank (part 10)
  swapped -> 2541. Its track stays the 2540 one (wwe13_entrance.VEHICLE_NODES).
- HCTP 2035 (the Undertaker's bike, 3003 / 3203 / 3411 / 3511) -> 2542 (hctp_bike.py: the PS2 model with
  its own nodes).
- Legends of WrestleMania 2552 / 2565 (the "Gondola" ring carts): LoW's own entries, as they are, under their own
  ids (both free in 2011's evtobj).
"""
import struct

import svrfmt

PROP_PAC = 'evt\\evtobj.pac'
PORTED = {('wwe2k14', 2540): 2541, ('hctp', 2035): 2542, ('low', 2552): 2552, ('low', 2565): 2565}


_unpack = svrfmt.unbpe


def game_prop(game_pac, pid):
    """2011's evtobj.pac SOBJ <pid> (unpacked PACH bytes) or None"""
    import os
    d = open(os.path.join(game_pac, 'evt', 'evtobj.pac'), 'rb').read()
    _, groups, _ = svrfmt.epac_read(d)
    for typ, es in groups:
        for n, b in es:
            if n.decode('latin-1').strip(' \0') == str(pid):
                return _unpack(b)
    return None


def _textures(part):
    """a texture bank (u32 count, 12 bytes, count x {name[16], ext[4], u32 size, u32 offset}) -> {name: dds}"""
    raw = _unpack(part)
    n = struct.unpack_from('<I', raw, 0)[0]
    out = {}
    for i in range(n):
        r = 16 + 32 * i
        name = raw[r:r + 16].split(b'\0')[0].decode('latin-1').lower()
        size, off = struct.unpack_from('<II', raw, r + 20)
        out[name] = raw[off:off + size]
    return out


def retextured(game_entry, src_entry):
    """2011's prop entry with the source's texture bank (part 10), when the source has every texture 2011's
    has under the same names and sizes and some differ; else None (nothing to gain, or not the same model)"""
    ours = dict(svrfmt.pach_read(game_entry))
    theirs = dict(svrfmt.pach_read(src_entry))
    if 10 not in ours or 10 not in theirs:
        return None
    a, b = _textures(ours[10]), _textures(theirs[10])
    if any(k not in b or len(b[k]) != len(v) for k, v in a.items()):
        return None
    if all(b[k] == v for k, v in a.items()):
        return None
    ours[10] = theirs[10]
    return svrfmt.pach_write([(k, ours[k]) for k in sorted(ours)])


def set_prop(evp, old, new):
    """a 2011 entrance script (EVP: '0FOP', big-endian) with its props (type 00000100 0) on model old moved to new"""
    d = bytearray(evp)
    if d[:4] != b'0FOP':
        return evp
    n = struct.unpack_from('>I', d, 0x40)[0] & 0xFFFF
    t = struct.unpack_from('>I', d, 0x48)[0] + 8
    for i in range(n):
        q = t + 32 * i
        model, t0, t1 = struct.unpack_from('>III', d, q + 4)
        if model == old and (t0, t1) == (0x100, 0):
            struct.pack_into('>I', d, q + 4, new)
    return bytes(d)


def write(pack_dir, props):
    """props [(id, game prop id, entry bytes)] -> pack_dir/props/<id>.bin + the pack.txt lines"""
    import os
    if not props:
        return
    os.makedirs(os.path.join(pack_dir, 'props'), exist_ok=True)
    lines = []
    for pid, gid, data in props:
        fn = 'props/%d.bin' % pid
        open(os.path.join(pack_dir, fn), 'wb').write(data)
        lines += ['pacentry %s SOBJ %d %s' % (PROP_PAC, pid, fn), 'propfallback %d %d' % (pid, gid)]
    with open(os.path.join(pack_dir, 'pack.txt'), 'a', newline='\r\n') as f:
        f.write('\n'.join(lines) + '\n')
