"""HCTP (PS2) disc -> the extract layout the room tools read (the 'HCTP Extract/_unpacked' tree):

    bg/stg_00NN/<id>.<ext>   BG.PAC group STG, entry 00NN (a PAC; nested PACs also as <id>/ folders)
    gm/GMMC_GOBJ/<id>.bin    GM.PAC group GMMC, entry GOBJ (gimmick tags)
    m/MOT_GAME/<id>.bin      M.PAC group MOT, entry GAME (backstage acts)

    py hctp_unpack.py <disc image or folder> <out folder> [room ...]
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import discfs  # noqa: E402
import svrfmt  # noqa: E402


def dpac(d):
    """DPAC header -> {(group, name): (offset, size)}; numbered groups (u16 n | 0x8000, u16 start sector,
    n x {u16 id, u16 size / 0x100}) name their entries '%04x'; named groups ({u16 n, u16 ?, n x {name4,
    u16 sector, u16 size / 0x100}}, GM.PAC's counts doubled) until a zero name."""
    out = {}
    off = 0x800
    while off < 0x4000 and d[off:off + 4] != bytes(4):
        tag = d[off:off + 4].decode('latin-1').strip()
        n, start = struct.unpack_from('<HH', d, off + 4)
        if n & 0x8000:
            n &= 0x7FFF
            pos = 0x4000 + start * 0x800
            for i in range(n):
                id_, sz = struct.unpack_from('<HH', d, off + 8 + 4 * i)
                out[(tag, '%04x' % id_)] = (pos, sz * 0x100)
                pos += (sz * 0x100 + 0x7FF) & ~0x7FF
            off += 8 + 4 * n
        else:
            # (the count is not the entry count - GM.PAC's GMEV says 204 for 107: the entries end at a zero
            # name or where the sectors go back, which is the next group's header)
            cnt, last = 0, -1
            while cnt < n and d[off + 8 + 8 * cnt:off + 12 + 8 * cnt] != bytes(4):
                p = off + 8 + 8 * cnt
                sec, sz = struct.unpack_from('<HH', d, p + 4)
                if sec < last:
                    break
                last = sec
                out[(tag, d[p:p + 4].decode('latin-1'))] = (0x4000 + sec * 0x800, sz * 0x100)
                cnt += 1
            off += 8 + 8 * cnt
    return out


def pac(d):
    n = struct.unpack_from('<I', d, 4)[0]
    base = 8 + 8 * n
    out = []
    for i in range(n):
        e = d[8 + 8 * i:16 + 8 * i]
        id_ = struct.unpack_from('<H', e)[0]
        o, s = int.from_bytes(e[2:5], 'little'), int.from_bytes(e[5:8], 'little')
        out.append((id_, d[base + o:base + o + s]))
    return out


unbpe = svrfmt.unbpe


def ext(b, id_):
    if b[:4] == b'YOBJ':
        return 'yobj'
    if b[:4] == b'HMD ':
        return 'hmd'
    if b[:4] == b'PAC ':
        return 'pac'
    if id_ in (50000, 50005):
        return 'txt'
    return 'bin'


def write_pac(data, folder):
    os.makedirs(folder, exist_ok=True)
    for id_, b in pac(data):
        b = unbpe(b)
        x = ext(b, id_)
        open(os.path.join(folder, '%05d.%s' % (id_, x)), 'wb').write(b)
        if x == 'pac':
            write_pac(b, os.path.join(folder, '%05d' % id_))


def entry(disc, path, group, name):
    head = disc.read(path, 0, 0x4000)
    if head[:4] != b'DPAC':
        raise ValueError('%s: not a DPAC archive' % path)
    off, size = dpac(head)[(group, name)]
    return unbpe(disc.read(path, off, size))


def unpack(disc, out, rooms):
    """The files the room tools need for these rooms (30 always: its bank is every room's base)."""
    for r in sorted(set(rooms) | {30}):
        write_pac(entry(disc, 'PAC/BG.PAC', 'STG', '%04d' % r), os.path.join(out, 'bg', 'stg_%04d' % r))
    write_pac(entry(disc, 'PAC/GM.PAC', 'GMMC', 'GOBJ'), os.path.join(out, 'gm', 'GMMC_GOBJ'))
    write_pac(entry(disc, 'PAC/M.PAC', 'MOT', 'GAME'), os.path.join(out, 'm', 'MOT_GAME'))


if __name__ == '__main__':
    a = sys.argv[1:]
    with discfs.open(a[0]) as d:
        unpack(d, a[1], [int(x) for x in a[2:]] or range(30, 37))
