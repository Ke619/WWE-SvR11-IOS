"""HCTP (PS2) archives: DPAC (top level, like 2011 EPAC) and PAC (inner, like PACH).

DPAC: "DPAC", u32 ?, u32 data size, u32 groups; groups from 0x800, data from 0x4000.
  group: 4CC, then either
    u16 n|0x8000, u16 start sector, n x {u16 id, u16 size/0x100} laid out from start
    (entries 0x800 aligned), or
    u32 n, n x {name4, u16 sector, u16 size/0x100}.
PAC : "PAC ", u32 n, n x {u16 id, u24 off, u24 size}; offsets from end of table.
"""
import struct
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import svrfmt  # noqa: E402

ZERO4 = bytes(4)


def dpac(d):
    """-> [(group, name, offset, size)]"""
    out = []
    off = 0x800
    while off < 0x4000 and d[off:off + 4] != ZERO4:
        tag = d[off:off + 4].decode('latin-1').strip()
        n, start = struct.unpack_from('<HH', d, off + 4)
        if n & 0x8000:
            n &= 0x7FFF
            pos = 0x4000 + start * 0x800
            for i in range(n):
                id_, sz = struct.unpack_from('<HH', d, off + 8 + 4 * i)
                out.append((tag, '%04x' % id_, pos, sz * 0x100))
                pos += (sz * 0x100 + 0x7FF) & ~0x7FF
            off += 8 + 4 * n
        else:
            cnt = 0
            for i in range(n):
                p = off + 8 + 8 * i
                if d[p:p + 4] == ZERO4:
                    break
                cnt += 1
                nm = d[p:p + 4].replace(b'\0', b'').decode('latin-1').strip()
                sec, sz = struct.unpack_from('<HH', d, p + 4)
                out.append((tag, nm, 0x4000 + sec * 0x800, sz * 0x100))
            off += 8 + 8 * cnt
    return out


def pac(d):
    if d[:4] != b'PAC ':
        return None
    n = struct.unpack_from('<I', d, 4)[0]
    base = 8 + 8 * n
    ents = []
    for i in range(n):
        e = d[8 + 8 * i:16 + 8 * i]
        id_ = struct.unpack_from('<H', e)[0]
        o = int.from_bytes(e[2:5], 'little')
        s = int.from_bytes(e[5:8], 'little')
        ents.append((id_, d[base + o: base + o + s]))
    return ents


unbpe = svrfmt.unbpe
