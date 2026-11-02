"""Character files (pac/ch/chNNN.pac): an "EPK8" archive (as EPAC, with
8-character entry names) holding one PACH of the character's parts.

    ch_tool.py list <chNNN.pac>...          entries, kinds, texture names
    ch_tool.py textures <chNNN.pac> <dir>   the texture bundles as .dds
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import svrfmt  # noqa: E402


def epk8_read(d):
    """-> (header, [(group type, [(name8, blob)])], trailer)"""
    if d[:4] != b'EPK8':
        raise ValueError('not EPK8')
    groups = []
    p = 0x800
    while p < 0x4000:
        typ = d[p:p + 4]
        if typ == b'\0\0\0\0':
            break
        cnt = struct.unpack_from('<H', d, p + 4)[0]
        p += 12
        ents = []
        for _ in range(cnt):
            name = d[p:p + 8]
            sec, sz = struct.unpack_from('<II', d, p + 8)
            off = 0x4000 + sec * 0x800
            ents.append((name, d[off:off + sz * 0x100]))
            p += 16
        groups.append((typ, ents))
    return d[:0x800], groups


unpack = svrfmt.unbpe


def is_bundle(raw):
    return len(raw) > 16 and struct.unpack_from('<I', raw, 4)[0] == 0x100 and struct.unpack_from('<I', raw, 12)[0] == 0x10


def bundle_names(raw):
    c = struct.unpack_from('<I', raw, 0)[0]
    return [raw[16 + 32 * k:32 + 32 * k].split(b'\0')[0].decode('latin1') for k in range(c)]


def walk(blob, depth, out):
    for i, data in svrfmt.pach_read(blob):
        raw = unpack(data)
        kind = raw[:4]
        if kind == b'PACH':
            out.append((depth, i, len(raw), 'PACH', ''))
            walk(raw, depth + 1, out)
        elif is_bundle(raw):
            out.append((depth, i, len(raw), 'TEX', ' '.join(bundle_names(raw))))
        else:
            out.append((depth, i, len(raw), kind.decode('latin1', 'replace'), ''))


def main():
    cmd = sys.argv[1]
    if cmd == 'list':
        for path in sys.argv[2:]:
            d = open(path, 'rb').read()
            h, groups = epk8_read(d)
            print(os.path.basename(path), len(d))
            for typ, ents in groups:
                for name, blob in ents:
                    print(' ', typ, name, len(blob))
                    out = []
                    walk(blob, 2, out)
                    for depth, i, n, kind, extra in out:
                        print(' ' * depth + '%04x %8d %-5s %s' % (i, n, kind, extra[:200]))
    elif cmd == 'textures':
        d = open(sys.argv[2], 'rb').read()
        h, groups = epk8_read(d)
        os.makedirs(sys.argv[3], exist_ok=True)
        n = 0
        for typ, ents in groups:
            for name, blob in ents:
                stack = [blob]
                while stack:
                    b = stack.pop()
                    for i, data in svrfmt.pach_read(b):
                        raw = unpack(data)
                        if raw[:4] == b'PACH':
                            stack.append(raw)
                        elif is_bundle(raw):
                            c = struct.unpack_from('<I', raw, 0)[0]
                            for k in range(c):
                                rec = raw[16 + 32 * k:48 + 32 * k]
                                nm = rec[:16].split(b'\0')[0].decode('latin1')
                                size, off = struct.unpack_from('<II', rec, 20)
                                open(os.path.join(sys.argv[3], '%04x_%s.dds' % (i, nm)), 'wb').write(raw[off:off + size])
                                n += 1
        print(n, 'textures')


if __name__ == '__main__':
    main()
