"""SvR2011 game-file formats: EPAC archives, PACH indexes, Yuke's BPE.

Arena mod work (branch `arenas`). Everything here is pure Python so the
tools run anywhere; the C++ `svrfmt` library mirrors it later.

EPAC (little-endian)
    0x00  "EPAC", u32 ?, u32 data size, u32 7
    0x800 groups: 4CC type, u32 entries*3, u32 0, then 12-byte entries
          {char name[4], u32 sector (0x800 bytes, from 0x4000?), u32 size/0x100}
PACH
    "PACH", u32 count, count x {u32 id, u32 offset, u32 size}; offsets are
    from the end of the table, entries are 4-byte aligned.
BPE
    "BPE ", u32 0x100, u32 packed size, u32 unpacked size, then Philip Gage
    byte-pair blocks (pair table, u16 LE length, data).
"""
import os
import struct

# ---------------------------------------------------------------- BPE

def bpe_decode(data):
    """Unpack one "BPE " entry. Returns bytes; raises on a bad header."""
    if data[:4] != b'BPE ':
        raise ValueError('not BPE')
    _, packed, unpacked = struct.unpack_from('<III', data, 4)
    src = data
    pos, end = 16, 16 + packed
    out = bytearray()
    while pos < end:
        left = list(range(256))
        right = [0] * 256
        c = 0
        count = src[pos]; pos += 1
        while True:
            if count > 127:
                c += count - 127
                count = 0
            if c == 256:
                break
            for _ in range(count + 1):
                left[c] = src[pos]; pos += 1
                if c != left[c]:
                    right[c] = src[pos]; pos += 1
                c += 1
            if c == 256:
                break
            count = src[pos]; pos += 1
        size = src[pos] | (src[pos + 1] << 8); pos += 2
        exp = [None] * 256

        def ex(s):
            e = exp[s]
            if e is None:
                if left[s] == s:
                    e = bytes((s,))
                else:
                    e = ex(left[s]) + ex(right[s])
                exp[s] = e
            return e
        block = src[pos:pos + size]; pos += size
        out += b''.join([ex(b) for b in block])
    if len(out) != unpacked:
        raise ValueError(f'BPE size {len(out)} != {unpacked}')
    return bytes(out)


def unbpe(data):
    """bpe_decode a "BPE " entry; anything else comes back as it is."""
    return bpe_decode(data) if data[:4] == b'BPE ' else data


_HERE = os.path.dirname(os.path.abspath(__file__))
# svrmod.exe beside this file (the player's "Mod Maker Tools" folder), else the
# developer build (modmaker/build_cli.sh -> port/out/modmaker)
# (SVR2011_SVRMOD: the one the Mod Maker hands every converter - its own Mod Maker Tools folder's - goes first)
_CLI = next((p for p in (os.environ.get('SVR2011_SVRMOD') or '', os.path.join(_HERE, 'svrmod.exe'),
                         os.path.join(_HERE, '..', 'out', 'modmaker', 'svrmod.exe')) if p and os.path.isfile(p)),
            os.path.join(_HERE, 'svrmod.exe'))


def bpe_pack(raw):
    """Real compression through the C++ encoder (svrmod.exe bpe). The game
    loads an entry in place: packed data must be smaller than unpacked."""
    import subprocess
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        a, b = os.path.join(d, 'raw'), os.path.join(d, 'bpe')
        open(a, 'wb').write(raw)
        subprocess.run([_CLI, 'bpe', a, b], check=True, capture_output=True)
        out = open(b, 'rb').read()
    if len(out) >= len(raw):
        raise ValueError(f'entry does not compress ({len(out)} >= {len(raw)}): the game would overflow')
    return out


def bpe_encode(raw, compress=False):
    """Pack bytes as a "BPE " entry.

    compress=False writes "stored" blocks: an empty pair table (skip 128,
    literal 128, skip 127) and the raw bytes, which any Gage decoder
    expands as-is.
    compress=True runs Gage's pair replacement (slow, pure Python)."""
    out = bytearray()
    step = 4000   # Yuke's blocks unpack to at most 4000 bytes (the game's buffer)
    for start in range(0, len(raw), step):
        chunk = raw[start:start + step]
        if compress:
            out += _bpe_block(chunk)
        else:
            out += bytes((255, 128, 254)) + struct.pack('<H', len(chunk)) + chunk
    return b'BPE ' + struct.pack('<III', 0x100, len(out), len(raw)) + bytes(out)


def _bpe_block(buf):
    buf = bytearray(buf)
    left = list(range(256))
    right = [0] * 256
    used = [False] * 256
    for b in buf:
        used[b] = True
    free = [k for k in range(256) if not used[k]]
    while free:
        counts = {}
        for i in range(len(buf) - 1):
            p = buf[i] | (buf[i + 1] << 8)
            counts[p] = counts.get(p, 0) + 1
        if not counts:
            break
        pair, best = max(counts.items(), key=lambda kv: kv[1])
        if best < 4:
            break
        code = free.pop(0)
        a, b = pair & 0xFF, pair >> 8
        nb = bytearray()
        i, n = 0, len(buf)
        while i < n:
            if i < n - 1 and buf[i] == a and buf[i + 1] == b:
                nb.append(code); i += 2
            else:
                nb.append(buf[i]); i += 1
        buf = nb
        left[code], right[code] = a, b
    return _bpe_table(left, right) + struct.pack('<H', len(buf)) + bytes(buf)


def _bpe_table(left, right):
    """Gage pair table. A byte >127 skips (byte-127) identity codes and is
    followed by one entry; a byte <=127 is followed by byte+1 entries.
    An entry is left[c], plus right[c] when left[c] != c."""
    out = bytearray()
    c = 0
    while c < 256:
        skip = 0
        while c + skip < 256 and left[c + skip] == c + skip and skip < 128:
            skip += 1
        if skip:
            out.append(127 + skip)
            c += skip
            if c == 256:
                break
            out.append(left[c])
            if left[c] != c:
                out.append(right[c])
            c += 1
            continue
        run = 0
        while c + run < 256 and left[c + run] != c + run and run < 128:
            run += 1
        out.append(run - 1)
        for k in range(c, c + run):
            out.append(left[k])
            out.append(right[k])
        c += run
    return bytes(out)


# ---------------------------------------------------------------- PACH

def pach_read(data):
    """-> list of (id, bytes) in file order. Data stays packed (BPE)."""
    if data[:4] != b'PACH':
        raise ValueError('not PACH')
    n = struct.unpack_from('<I', data, 4)[0]
    ents = [struct.unpack_from('<III', data, 8 + 12 * i) for i in range(n)]
    base = 8 + 12 * n
    return [(i, data[base + o:base + o + s]) for i, o, s in ents]


def pach_write(entries):
    n = len(entries)
    table = bytearray(b'PACH' + struct.pack('<I', n))
    body = bytearray()
    for eid, blob in entries:
        table += struct.pack('<III', eid, len(body), len(blob))
        body += blob
        body += b'\0' * (-len(body) % 4)
    return bytes(table + body)


# ---------------------------------------------------------------- EPAC

def epac_read(data):
    """-> (header, groups, trailer). header = bytes 0..0x800, groups =
    [(type, [(name, blob)])], trailer = the packer's 0x800-byte footer
    ("EOP5/plugin version ..."), kept so a repack is byte-identical."""
    if data[:4] != b'EPAC':
        raise ValueError('not EPAC')
    groups = []
    p = 0x800
    while p < 0x4000:
        typ = data[p:p + 4]
        if typ == b'\0\0\0\0':
            break
        # (entries x 3 in the low 16 bits; the high half is a per-file constant
        # in SvR 2007 pacs and a flag in WWE '13's EPK8)
        cnt = (struct.unpack_from('<I', data, p + 4)[0] & 0xFFFF) // 3
        p += 12
        ents = []
        for _ in range(cnt):
            name = data[p:p + 4]
            sec, sz = struct.unpack_from('<II', data, p + 4)
            off = 0x4000 + sec * 0x800
            ents.append((name, data[off:off + sz * 0x100]))
            p += 12
        groups.append((typ, ents))
    size = struct.unpack_from('<I', data, 8)[0]
    return data[:0x800], groups, data[0x4000 + size:]


def epac_write(header, groups, trailer=b''):
    """Rebuild an EPAC: keeps header bytes 0..0x800 (except the table and
    data sizes) and lays entries out on 0x800 sectors from 0x4000, in the
    given order. +4 is the table's size in bytes: the game reads only that
    much of it, so an added entry past it is never found."""
    out = bytearray(header[:0x800])
    toc = bytearray()
    body = bytearray()
    for typ, ents in groups:
        toc += typ + struct.pack('<II', len(ents) * 3, 0)
        for name, blob in ents:
            body += b'\0' * (-len(body) % 0x800)
            toc += name + struct.pack('<II', len(body) // 0x800,
                                      (len(blob) + 0xFF) // 0x100)
            body += blob
            body += b'\0' * (-len(body) % 0x100)
    body += b'\0' * (-len(body) % 0x800)
    if len(toc) > 0x3800:
        raise ValueError('EPAC table too big')
    out += toc + b'\0' * (0x3800 - len(toc))
    struct.pack_into('<I', out, 4, len(toc))
    struct.pack_into('<I', out, 8, len(body))
    return bytes(out + body) + (trailer or bytes(0x800))
