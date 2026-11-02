"""SvR 2011 YMKs motion banks: reader / writer, frame stream, body pose codecs
(m.pac 'unpacked' records and the MOTP/GAME 'packed' bit codec), events, cameras.

Everything here was decoded from the recompiled game code (see YMKS_SPEC.md next to
this file for the field-by-field spec and the code addresses). All multi-byte
values are little-endian.

Layers (each has decode + encode that round-trip byte-exactly on stock data):

  read_bank(raw) -> Bank            write_bank(Bank) -> bytes
  parse_stream(data) -> Stream      build_stream(Stream) -> bytes
  split_record(rec, packed) -> (events_bytes, pose_bytes)   join_record(...)
  parse_events(ev, T) / build_events(list)
  decode_body(pose) -> dict         encode_body(dict) -> bytes          (m.pac, 0x7d...)
  decode_packed(pose) -> dict       encode_packed(dict) -> bytes        (MOTP/GAME)
  decode_camera(pose) / encode_camera(dict)                             (29/13/9-byte camera tracks)

Bone values are kept as the raw 16-bit codes the game stores (angle = s16 * 2pi/65536,
Euler order R = Rz*Ry*Rx, the same convention as YMBs; see bone_angles()).
"""
import math
import struct

# ----------------------------------------------------------------------------- constants

BONES = ['root', 'koshi', 'mune', 'kubi', 'atama',
         'l_sakotsu', 'l_ninoude', 'l_kote', 'l_te',
         'r_sakotsu', 'r_ninoude', 'r_kote', 'r_te',
         'l_momo', 'l_sune', 'l_ashi', 'l_tsumasaki',
         'r_momo', 'r_sune', 'r_ashi', 'r_tsumasaki']

# per-bone "type" byte (bone struct +372), table at 0x82DA0AF0, fixed up by sub_8238BBA0:
#   bit0 x, bit1 y, bit2 z present; bit3 = 8-bit storage; 0x17 = xyz (ashi, atama)
TYPES_TABLE = [7, 7, 7, 14, 0x17, 6, 7, 3, 14, 6, 7, 3, 14, 7, 2, 0x17, 10, 7, 2, 0x17, 10, 7, 7, 7, 7]
# mode 1 (m.pac / evt banks: every value 16-bit): 10 -> 2, 14 -> 6, ashi stays 0x17
TYPES_M1 = [2 if t == 10 else 6 if t == 14 else t for t in TYPES_TABLE]
# mode 3 (MOTP/GAME packed): ashi (15, 19) -> 14
TYPES_PK = list(TYPES_TABLE)
TYPES_PK[15] = TYPES_PK[19] = 14

ANG = 2 * math.pi / 65536.0          # s16 angle unit (0x82002854)
ROOT_UNIT = 0.01                     # root translation unit (0x820C0D2C), model units

# packed codec bit widths (sub_8238D330 defaults are 12/12/8/7; MOTP characters use these)
PK_ROOT_BITS = 12       # obj+92
PK_ANG_BITS = 9         # obj+96   N
PK_P_BITS = 7           # obj+100  P  (second value of type 14 uses P-1 bits)
PK_M_BITS = 5           # obj+104  M  (types 10 and 14)
PK_KUBI_SKIP = True     # obj+88: kubi is not sent in the full (0x80) form


def axes_of(t):
    """axes a bone type carries in the unpacked format, in storage order"""
    return {2: 'y', 3: 'xy', 4: 'z', 6: 'yz', 10: 'y', 14: 'yz'}.get(t, 'xyz')


def bone_size(t):
    """bytes of one bone in the unpacked format (sub_82390740)"""
    return {2: 2, 3: 4, 4: 2, 6: 4, 10: 1, 14: 2}.get(t, 6)


def s16(v):
    v &= 0xffff
    return v - 0x10000 if v & 0x8000 else v


# ----------------------------------------------------------------------------- bank

class Bank:
    """head: bytes 0..0x110 ('YMKs', u32 0x100, 8 zero bytes, T[256] event-size table)
    entries: list of dict(y, x, id, frames, rt, data, share) in directory order.
    share=True: entry uses the same data offset as the previous entry (data is then the
    same bytes object). tail_off: for entries whose offset lies at/after the end of data
    (stock quirk), the stored distance past the end."""
    def __init__(self, head, entries):
        self.head = head
        self.entries = entries

    @property
    def T(self):
        return self.head[0x10:0x110]

    def find(self, mid, x=0, y=0):
        for e in self.entries:
            if e['id'] == mid and e['x'] == x and e['y'] == y:
                return e
        return None


def read_bank(raw):
    assert raw[:4] in (b'YMKs', b'YMBs'), raw[:4]
    n = struct.unpack_from('<I', raw, 0x110)[0]
    base = 0x114 + 16 * n
    dl = len(raw) - base
    dirs = [struct.unpack_from('<BBHIII', raw, 0x114 + 16 * i) for i in range(n)]
    offs = sorted(set(d[3] for d in dirs if d[3] < dl))
    if not offs or offs[0] != 0:
        offs = [0] + offs                       # leading bytes not owned by any entry
    nxt = {o: (offs[i + 1] if i + 1 < len(offs) else dl) for i, o in enumerate(offs)}
    lead = raw[base:base + offs[0]] if offs[0] else b''
    ents = []
    prev_off = None
    for y, x, mid, off, fr, rt in dirs:
        e = dict(y=y, x=x, id=mid, frames=fr, rt=rt, share=False, tail_off=None)
        if off >= dl:
            e['data'] = b''
            e['tail_off'] = off - dl
        elif off == prev_off:
            e['data'] = ents[-1]['data']
            e['share'] = True
        else:
            e['data'] = raw[base + off:base + nxt[off]]
        prev_off = off
        ents.append(e)
    b = Bank(raw[:0x110], ents)
    b.lead = lead
    return b


def write_bank(bank):
    """offsets are laid out in directory order (the game needs them monotonic: the
    per-superstar bank gatherer sizes an entry as next.off - off)"""
    body = bytearray(getattr(bank, 'lead', b''))
    offs = []
    pend = []
    for e in bank.entries:
        if e.get('tail_off') is not None:
            pend.append(len(offs))
            offs.append(None)
            continue
        if e.get('share') and offs:
            offs.append(offs[-1])
            continue
        offs.append(len(body))
        body += e['data']
    for i in pend:
        offs[i] = len(body) + bank.entries[i]['tail_off']
    out = bytearray(bank.head[:0x110])
    out += struct.pack('<I', len(bank.entries))
    for e, o in zip(bank.entries, offs):
        out += struct.pack('<BBHIII', e['y'], e['x'], e['id'], o, e['frames'], e['rt'])
    out += body
    return bytes(out)


# ----------------------------------------------------------------------------- frame stream

class Stream:
    """items: list of ('skip', n) [one byte, n < 0x60] or ('rec', bytes, long) where long=True
    means the size was written as 0x60 + u16 (the game uses it for size >= 0xa0)."""
    def __init__(self, items):
        self.items = items

    def records(self):
        """-> list of (frame, record bytes)"""
        f = 0
        out = []
        for it in self.items:
            if it[0] == 'skip':
                f += it[1]
            else:
                out.append((f, it[1]))
                f += 1
        return out

    def nframes(self):
        f = 0
        for it in self.items:
            f += it[1] if it[0] == 'skip' else 1
        return f


def parse_stream(d):
    """sub_82394360: b < 0x60 skips b frames; 0x60 u16 = record size; b > 0x60 = size b-0x60.
    Every record occupies one frame."""
    p = 0
    items = []
    while p < len(d):
        b = d[p]
        if b < 0x60:
            items.append(('skip', b))
            p += 1
            continue
        if b == 0x60:
            sz = d[p + 1] | (d[p + 2] << 8)
            p += 3
            lng = True
        else:
            sz = b - 0x60
            p += 1
            lng = False
        if p + sz > len(d):
            raise ValueError('record overruns motion data')
        items.append(('rec', d[p:p + sz], lng))
        p += sz
    return Stream(items)


def build_stream(st):
    out = bytearray()
    for it in st.items:
        if it[0] == 'skip':
            assert 0 <= it[1] < 0x60
            out.append(it[1])
        else:
            r = it[1]
            lng = it[2] if len(it) > 2 else len(r) > 0x9f
            if lng or len(r) == 0:
                out += bytes([0x60]) + struct.pack('<H', len(r))
            else:
                assert len(r) <= 0x9f
                out.append(0x60 + len(r))
            out += r
    return bytes(out)


def make_stream(frame_records, nframes):
    """frame_records: sorted list of (frame, record bytes) -> Stream with canonical skips
    (skips split into 0x5f chunks) ending at nframes."""
    items = []
    f = 0
    for fr, r in frame_records:
        gap = fr - f
        assert gap >= 0
        while gap > 0:
            n = min(gap, 0x5f)
            items.append(('skip', n))
            gap -= n
        items.append(('rec', r, len(r) > 0x9f))
        f = fr + 1
    gap = nframes - f
    while gap > 0:
        n = min(gap, 0x5f)
        items.append(('skip', n))
        gap -= n
    return Stream(items)


# ----------------------------------------------------------------------------- records + events

def split_record(r, packed=False):
    """-> (event block bytes, pose bytes).
    unpacked (m.pac): first byte with bit 7 = event block of (b & 0x7f) bytes.
    packed (MOTP character tracks): first byte < 0x20 = event block of b+1 bytes."""
    if not r:
        return b'', b''
    b = r[0]
    if packed:
        if b < 0x20:
            return r[1:2 + b], r[2 + b:]
        return b'', r
    if b & 0x80:
        n = b & 0x7f
        return r[1:1 + n], r[1 + n:]
    return b'', r


def join_record(ev, pose, packed=False):
    if not ev:
        return bytes(pose)
    if packed:
        assert 1 <= len(ev) <= 0x20
        return bytes([len(ev) - 1]) + ev + pose
    assert len(ev) <= 0x7f
    return bytes([0x80 | len(ev)]) + ev + pose


def parse_events(ev, T):
    """event block -> list of (op, bytes incl. op). Each event is T[op] bytes long
    (sub_8239D750); T = bank head[0x10:0x110]. Returns (list, ok)."""
    out = []
    p = 0
    while p < len(ev):
        op = ev[p]
        n = T[op]
        if n == 0 or p + n > len(ev):
            return out + [(None, ev[p:])], False
        out.append((op, ev[p:p + n]))
        p += n
    return out, True


def build_events(lst):
    return b''.join(b for op, b in lst)


# ----------------------------------------------------------------------------- unpacked body (m.pac)

def _rd_bone(b, p, t):
    """sub_82390740: one bone in the unpacked format -> dict axis -> s16 code"""
    if t == 10:                                  # y, 8-bit (high byte)
        return {'y': s16(b[p] << 8)}, 1
    if t == 14:                                  # y z, 8-bit
        return {'y': s16(b[p] << 8), 'z': s16(b[p + 1] << 8)}, 2
    ax = axes_of(t)
    n = bone_size(t) // 2
    v = struct.unpack_from('<%dh' % n, b, p)
    return dict(zip(ax, v)), 2 * n


def _wr_bone(vals, t):
    if t == 10:
        return bytes([(vals.get('y', 0) >> 8) & 0xff])
    if t == 14:
        return bytes([(vals.get('y', 0) >> 8) & 0xff, (vals.get('z', 0) >> 8) & 0xff])
    return b''.join(struct.pack('<h', s16(vals.get(a, 0))) for a in axes_of(t))


def decode_body(pose, types=TYPES_M1):
    """unpacked character pose (sub_82391380) -> dict(subs=[...], trail=bytes).
    Each sub-record dict has 'op' and, as present:
      count  : blend length; the pose is reached in (count+1)*2 ticks = count+1 frames
      root   : (x, y, z) in 0.01 model units; root_fmt 'f32' or 's16'
      mask   : bone mask (masked forms)
      bones  : {bone index: {axis: s16 code}}
      ref    : 0x7b/0x76 reference index, raw: undecoded bytes
    A stock body record is one sub-record: op 0x7d (110 bytes: count, f32 root, all 21
    bones) or 0x50-0x5f (f32 root + mask)."""
    subs = []
    p = 0
    n = len(pose)
    while p < n - 1:                             # the game loops while offset < size-1
        try:
            s, q = _body_sub(pose, p, types)
        except (IndexError, struct.error):
            q = n + 1
        if q > n:
            if not subs:
                raise ValueError('body sub-record overruns the record')
            break                                # stock quirk: leftover bytes the game reads past
        subs.append(s)
        p = q
    return {'subs': subs, 'trail': pose[p:]}


def _body_sub(pose, p, types):
    if True:
        op = pose[p]
        s = {'op': op}
        if op in (0x7e, 0x7f):
            s['count'] = pose[p + 1]
            if op == 0x7e:
                s['root'] = struct.unpack_from('<3h', pose, p + 2); s['root_fmt'] = 's16'; p += 8
            else:
                s['root'] = struct.unpack_from('<3f', pose, p + 2); s['root_fmt'] = 'f32'; p += 14
        elif op in (0x7b, 0x76):
            s['ref'] = pose[p + 1]
            s['count'] = pose[p + 2]
            if op == 0x7b:
                s['root'] = struct.unpack_from('<3h', pose, p + 3); s['root_fmt'] = 's16'; p += 9
            else:
                s['raw'] = pose[p + 3:p + 7]; p += 7
        elif op == 0x74:
            q = p + 1
            a = pose[q]; q += 1
            if a:
                q += 2
            c = pose[q]; q += 1
            if c:
                q += 4
            s['raw'] = pose[p + 1:q]
            p = q
        elif op in (0x7d, 0x7c) or 0x60 <= op <= 0x6f:
            if op == 0x7d:
                s['count'] = pose[p + 1]; s['root'] = struct.unpack_from('<3f', pose, p + 2); s['root_fmt'] = 'f32'; q = p + 14
            elif op == 0x7c:
                s['count'] = pose[p + 1]; s['root'] = struct.unpack_from('<3h', pose, p + 2); s['root_fmt'] = 's16'; q = p + 8
            else:
                s['count'] = op - 0x60; s['root'] = struct.unpack_from('<3h', pose, p + 1); s['root_fmt'] = 's16'; q = p + 7
            s['bones'] = {}
            for k in range(21):
                s['bones'][k], ln = _rd_bone(pose, q, types[k]); q += ln
            p = q
        elif op in (0x7a, 0x75) or 0x50 <= op <= 0x5f:
            if op == 0x7a:
                s['count'] = pose[p + 1]; s['root'] = struct.unpack_from('<3f', pose, p + 2); s['root_fmt'] = 'f32'; q = p + 14
            elif op == 0x75:
                s['count'] = pose[p + 1]; s['root'] = struct.unpack_from('<3h', pose, p + 2); s['root_fmt'] = 's16'; q = p + 8
            else:
                s['count'] = op - 0x50; s['root'] = struct.unpack_from('<3f', pose, p + 1); s['root_fmt'] = 'f32'; q = p + 13
            m = pose[q] | pose[q + 1] << 8 | pose[q + 2] << 16
            q += 3
            s['mask'] = m
            s['bones'] = {}
            for k in range(21):
                if m >> k & 1:
                    s['bones'][k], ln = _rd_bone(pose, q, types[k]); q += ln
            p = q
        elif op <= 0x7a and op < len(types):     # single bone: [bone][count][data]
            s['count'] = pose[p + 1]
            v, ln = _rd_bone(pose, p + 2, types[op])
            s['bones'] = {op: v}
            p += 2 + ln
        else:
            raise ValueError('unknown body sub-record op 0x%02x' % op)
        return s, p


def encode_body(d, types=TYPES_M1):
    out = bytearray()
    for s in d['subs']:
        op = s['op']
        if op in (0x7e, 0x7f):
            out += bytes([op, s['count']])
            out += struct.pack('<3h', *s['root']) if op == 0x7e else struct.pack('<3f', *s['root'])
        elif op == 0x7b:
            out += bytes([op, s['ref'], s['count']]) + struct.pack('<3h', *s['root'])
        elif op == 0x76:
            out += bytes([op, s['ref'], s['count']]) + bytes(s['raw'])
        elif op == 0x74:
            out += bytes([op]) + bytes(s['raw'])
        elif op in (0x7d, 0x7c) or 0x60 <= op <= 0x6f or op in (0x7a, 0x75) or 0x50 <= op <= 0x5f:
            full = op in (0x7d, 0x7c) or 0x60 <= op <= 0x6f
            if op in (0x7d, 0x7a):
                out += bytes([op, s['count']]) + struct.pack('<3f', *s['root'])
            elif op in (0x7c, 0x75):
                out += bytes([op, s['count']]) + struct.pack('<3h', *s['root'])
            elif 0x60 <= op <= 0x6f:
                out += bytes([op]) + struct.pack('<3h', *s['root'])
            else:
                out += bytes([op]) + struct.pack('<3f', *s['root'])
            if full:
                ks = range(21)
            else:
                m = s.get('mask')
                if m is None:
                    m = sum(1 << k for k in s['bones'])
                out += bytes([m & 0xff, m >> 8 & 0xff, m >> 16 & 0xff])
                ks = [k for k in range(21) if m >> k & 1]
            for k in ks:
                out += _wr_bone(s['bones'].get(k, {}), types[k])
        else:
            k = op
            out += bytes([op, s['count']]) + _wr_bone(s['bones'][k], types[k])
    out += d.get('trail', b'')
    return bytes(out)


def full_pose(count, root, bones, types=TYPES_M1):
    """helper: one 0x7d record (110 bytes for characters). root in 0.01 units (floats),
    bones {k: {axis: s16}} (missing axes/bones = 0)."""
    return encode_body({'subs': [{'op': 0x7d, 'count': count, 'root': root, 'root_fmt': 'f32',
                                  'bones': bones}], 'trail': b''}, types)


# ----------------------------------------------------------------------------- packed body (MOTP)

class _BR:
    def __init__(self, b, pos):
        self.v = int.from_bytes(b, 'little')
        self.pos = pos

    def get(self, n):
        x = (self.v >> self.pos) & ((1 << n) - 1)
        self.pos += n
        return x


class _BW:
    def __init__(self):
        self.v = 0
        self.pos = 0

    def put(self, x, n):
        assert 0 <= x < (1 << n), (x, n)
        self.v |= x << self.pos
        self.pos += n


def _pk_fields(t, N=PK_ANG_BITS, M=PK_M_BITS, P=PK_P_BITS):
    """packed fields of a bone type: list of (axis, bits, shift, add, hibyte)
    value16 = ((code << shift) + add) & 0xffff ; hibyte -> & 0xff00 (8-bit types)"""
    if t == 4:
        return [('z', N, 16 - N, 0, False)]
    if t == 3:
        return [('x', N, 16 - N, 0, False), ('y', N - 1, 16 - N, 0, False)]
    if t == 2:
        return [('y', N - 1, 16 - N, 0x8000, False)]
    if t == 6:
        return [('y', N - 2, 16 - N, 0xE000, False), ('z', N - 2, 16 - N, 0xE000, False)]
    if t == 10:
        return [('y', M, 15 - M, -0x4000, True)]
    if t == 14:
        return [('y', M, 15 - M, 0xC000, True), ('z', P - 1, 16 - P, 0xC000, True)]
    return [('x', N, 16 - N, 0, False), ('y', N, 16 - N, 0, False), ('z', N, 16 - N, 0, False)]


def pk_code_to_s16(code, f):
    ax, bits, sh, add, hb = f
    v = ((code << sh) + add) & 0xffff
    if hb:
        v &= 0xff00
    return s16(v)


def pk_s16_to_code(val, f, rounding='floor'):
    """quantise an s16 angle code to a packed field. The stock MOTP encoder truncates
    (floor; MOTP vs m.pac error is always < one step), 'nearest' halves the error.
    Values outside the field's range clamp to the nearer end."""
    ax, bits, sh, add, hb = f
    u = (val - add) & 0xffff
    if rounding == 'nearest' and sh > 0:
        u = min(u + (1 << (sh - 1)), 0xffff)
    c = u >> sh
    hi = (1 << bits) - 1
    if c > hi:
        top = (hi + 1) << sh
        c = hi if (u - top) < (0x10000 - u) else 0
    return c


def decode_packed(pose, types=TYPES_PK, N=PK_ANG_BITS, M=PK_M_BITS, P=PK_P_BITS,
                  R=PK_ROOT_BITS, kubi_skip=PK_KUBI_SKIP):
    """packed character pose (sub_82390990) -> dict(subs=[...], trail=bytes).
    Sub-record forms (first byte h):
      0x3f            [3f][count][s16 root x3]                 root only
      0x80 | c        full: count=c, root + bones 0..20 (kubi not sent if kubi_skip)
      0x40 | lo       [h][m0][m1][m2]: mask = m0|m1<<8|(m2&0x1f)<<16, root if m2&0x20,
                      count = (m2 & 0xc0) | (lo & 0x3f)
      0x20 | k        [h][count]: one bone k, no root
    then an LSB-first bit stream: root R bits x3 (signed, * 0.02 units when R == 12),
    then each sent bone's fields (_pk_fields). Ends on a byte boundary.
    Each sub dict keeps the raw 'codes' (exact) plus 'bones' (s16 values) and 'root'
    (in 0.01 units, as the unpacked format)."""
    subs = []
    p = 0
    n = len(pose)
    while p < n - 1:
        h = pose[p]
        s = {'op': h}
        if h == 0x3f:
            s['form'] = 'root'
            s['count'] = pose[p + 1]
            s['root'] = struct.unpack_from('<3h', pose, p + 2)
            p += 8
            subs.append(s)
            continue
        if h & 0x80:
            s['form'] = 'full'
            s['count'] = h & 0x7f
            mask = 0x1FFFFF
            has_root = True
            start = 8
        elif h & 0x40:
            s['form'] = 'mask'
            m2 = pose[p + 3]
            mask = pose[p + 1] | pose[p + 2] << 8 | (m2 & 0x1f) << 16
            has_root = bool(m2 & 0x20)
            s['count'] = (m2 & 0xc0) | (h & 0x3f)
            start = 32
        elif h & 0x20:
            s['form'] = 'bone'
            mask = 1 << (h & 0x1f)
            has_root = False
            s['count'] = pose[p + 1]
            start = 16
        else:
            raise ValueError('bad packed header 0x%02x' % h)
        s['mask'] = mask
        br = _BR(pose[p:], start)
        if has_root:
            rc = [br.get(R) for _ in range(3)]
            s['root_codes'] = rc
            sc = 2 if R == 12 else 1
            s['root'] = tuple((c - (1 << R) if c >= (1 << (R - 1)) else c) * sc for c in rc)
        codes = {}
        bones = {}
        for k in range(25):
            if not (mask >> k & 1):
                continue
            if kubi_skip and k == 3 and s['form'] == 'full':
                continue
            fl = _pk_fields(types[k], N, M, P)
            cs = [br.get(f[1]) for f in fl]
            codes[k] = cs
            bones[k] = {f[0]: pk_code_to_s16(c, f) for f, c in zip(fl, cs)}
        if kubi_skip and s['form'] == 'full':
            bones[3] = {'y': 0, 'z': 0}          # not sent; the game zeroes kubi's target
        s['codes'] = codes
        s['bones'] = bones
        nb = (br.pos + 7) // 8
        s['nbytes'] = nb
        s['padbits'] = (br.v >> br.pos) & ((1 << (nb * 8 - br.pos)) - 1)
        if p + nb > n:
            raise ValueError('packed sub-record overruns the record')
        p += nb
        subs.append(s)
    return {'subs': subs, 'trail': pose[p:]}


def encode_packed(d, types=TYPES_PK, N=PK_ANG_BITS, M=PK_M_BITS, P=PK_P_BITS,
                  R=PK_ROOT_BITS, kubi_skip=PK_KUBI_SKIP, rounding='floor'):
    """inverse of decode_packed. Uses 'codes'/'root_codes' when present (exact),
    otherwise quantises 'bones' s16 values and 'root' (0.01 units)."""
    out = bytearray()
    for s in d['subs']:
        form = s.get('form')
        if form == 'root':
            out += bytes([0x3f, s['count']]) + struct.pack('<3h', *s['root'])
            continue
        cnt = s['count']
        if form == 'full':
            assert cnt < 0x80
            hdr = bytes([0x80 | cnt]); mask = 0x1FFFFF; has_root = True; start = 8
        elif form == 'mask':
            mask = s.get('mask')
            if mask is None:
                mask = sum(1 << k for k in s['bones'])
            has_root = 'root' in s or 'root_codes' in s
            hdr = bytes([0x40 | (cnt & 0x3f), mask & 0xff, mask >> 8 & 0xff,
                         (mask >> 16 & 0x1f) | (0x20 if has_root else 0) | (cnt & 0xc0)])
            start = 32
        else:   # bone
            (k,) = list(s['bones'].keys()) if 'bones' in s else list(s['codes'].keys())
            mask = 1 << k; has_root = False
            hdr = bytes([0x20 | k, cnt]); start = 16
        bw = _BW()
        bw.pos = start
        if has_root:
            rc = s.get('root_codes')
            if rc is None:
                sc = 2 if R == 12 else 1
                rc = []
                for v in s['root']:
                    c = int(v / sc)              # stock encoder truncates toward zero
                    c = max(-(1 << (R - 1)), min((1 << (R - 1)) - 1, c))
                    rc.append(c & ((1 << R) - 1))
            for c in rc:
                bw.put(c, R)
        codes = s.get('codes', {})
        for k in range(25):
            if not (mask >> k & 1):
                continue
            if kubi_skip and k == 3 and form == 'full':
                continue
            fl = _pk_fields(types[k], N, M, P)
            cs = codes.get(k)
            if cs is None:
                bv = s['bones'].get(k, {})
                cs = [pk_s16_to_code(bv.get(f[0], 0), f, rounding) for f in fl]
            for f, c in zip(fl, cs):
                bw.put(c, f[1])
        nb = (bw.pos + 7) // 8
        pad = s.get('padbits', 0)
        if pad:
            bw.v |= pad << bw.pos
        body = bw.v.to_bytes(nb, 'little')
        out += hdr + body[len(hdr):]
    out += d.get('trail', b'')
    return bytes(out)


def pack_from_body(dbody, keep_count=True):
    """convert one decoded unpacked character pose (decode_body) to a packed dict
    (full form, quantised). The root must fit in 12 bits * 0.02 (|v| < 40.96 units)."""
    s0 = [s for s in dbody['subs'] if 'bones' in s and len(s['bones']) == 21][0]
    return {'subs': [{'form': 'full', 'count': min(s0['count'], 0x7f) if keep_count else 0,
                      'root': s0['root'], 'bones': s0['bones']}], 'trail': b''}


# ----------------------------------------------------------------------------- camera tracks

def decode_camera(pose):
    """camera key (sub_8239DB28), chosen by size: 29 = mode 1 (m.pac), 13 = mode 0, 9 = mode 2
    (MOTP). -> dict(count, pos (model units), rot (rad, 3), fov (rad, mode 1 only), raw codes)"""
    c = pose[0]
    if len(pose) == 29:
        pos = struct.unpack_from('<3f', pose, 1)
        rot = struct.unpack_from('<3f', pose, 13)
        fov = struct.unpack_from('<f', pose, 25)[0]
        return dict(mode=1, count=c, pos=tuple(v * ROOT_UNIT for v in pos), rot=rot, fov=fov,
                    raw=(pos, rot, fov))
    if len(pose) == 13:
        pos = struct.unpack_from('<3h', pose, 1)
        rot = struct.unpack_from('<3h', pose, 7)
        return dict(mode=0, count=c, pos=tuple(v * ROOT_UNIT for v in pos),
                    rot=tuple(v * ANG for v in rot), raw=(pos, rot))
    if len(pose) == 9:
        w, r = struct.unpack_from('<II', pose, 1)
        sx = lambda v, b: v - (1 << b) if v >> (b - 1) & 1 else v
        pos = (sx(w & 0x7ff, 11), sx(w >> 11 & 0x3ff, 10), sx(w >> 21, 11))
        rot = (s16((r & 0x7ff) << 5), s16((r >> 11 & 0x7ff) << 5), s16((r >> 22) << 6))
        return dict(mode=2, count=c, pos=tuple(v * 0.16 for v in pos),
                    rot=tuple(v * ANG for v in rot), raw=(w, r))
    raise ValueError('unknown camera key size %d' % len(pose))


def encode_camera(cam):
    c = cam['count']
    if cam['mode'] == 1:
        pos, rot, fov = cam['raw'] if 'raw' in cam else (
            tuple(v / ROOT_UNIT for v in cam['pos']), cam['rot'], cam['fov'])
        return bytes([c]) + struct.pack('<3f3ff', *pos, *rot, fov)
    if cam['mode'] == 0:
        pos, rot = cam['raw'] if 'raw' in cam else (
            tuple(int(round(v / ROOT_UNIT)) for v in cam['pos']),
            tuple(s16(int(round(v / ANG))) for v in cam['rot']))
        return bytes([c]) + struct.pack('<3h3h', *pos, *rot)
    if cam['mode'] == 2:
        if 'raw' in cam:
            w, r = cam['raw']
        else:
            px, py, pz = (int(round(v / 0.16)) for v in cam['pos'])
            w = (px & 0x7ff) | (py & 0x3ff) << 11 | (pz & 0x7ff) << 21
            a = [int(round(v / ANG)) & 0xffff for v in cam['rot']]
            r = (a[0] >> 5) | (a[1] >> 5) << 11 | (a[2] >> 6) << 22
        return bytes([c]) + struct.pack('<II', w, r)
    raise ValueError(cam['mode'])


# ----------------------------------------------------------------------------- motion level helpers

def is_packed_motion(data, T):
    """MOTP character tracks use the packed codec; other tracks in MOTP (cameras, props,
    y=2, ring/rope objects) keep the unpacked conventions. Test: every record parses as
    packed (events ok with T, sub-records end exactly)."""
    try:
        st = parse_stream(data)
    except ValueError:
        return False
    any_pose = False
    for f, r in st.records():
        ev, pose = split_record(r, packed=True)
        if ev and not parse_events(ev, T)[1]:
            return False
        if pose:
            try:
                d = decode_packed(pose)
            except (ValueError, IndexError):
                return False
            if d['trail'] and len(d['trail']) > 1:
                return False
            any_pose = True
    return True          # event-only motions included (call this only for MOTP tracks y < 3)


def decode_motion(data, T, packed=None, types=None, y=0):
    """-> list of dict(frame, events=[(op, bytes)], ev_ok, kind, pose=decoded|bytes)
    kind: 'packed', 'body', or 'raw' (pose kept as bytes: cameras/props/other objects).
    packed=None: auto (only MOTP character tracks are packed; never tracks y >= 3)."""
    if packed is None:
        packed = y < 3 and is_packed_motion(data, T)
    st = parse_stream(data)
    out = []
    for f, r in st.records():
        ev, pose = split_record(r, packed)
        evl, ok = parse_events(ev, T)
        item = dict(frame=f, events=evl, ev_ok=ok, kind='raw', pose=pose)
        if pose:
            try:
                if packed:
                    item['pose'] = decode_packed(pose, types or TYPES_PK)
                    item['kind'] = 'packed'
                elif pose[0] in (0x7d, 0x7c, 0x7a, 0x75, 0x7e, 0x7f) or 0x50 <= pose[0] <= 0x6f:
                    d = decode_body(pose, types or TYPES_M1)
                    if encode_body(d, types or TYPES_M1) == pose:
                        item['pose'] = d
                        item['kind'] = 'body'
            except (ValueError, IndexError, struct.error):
                pass
        out.append(item)
    return out


def bone_angles(bvals):
    """{axis: s16} -> (rx, ry, rz) radians; missing axes 0. Local rotation R = Rz*Ry*Rx."""
    return tuple(bvals.get(a, 0) * ANG for a in 'xyz')


# ----------------------------------------------------------------------------- writing helpers

def key_of(mid, x, y):
    return (mid << 16) | (x << 8) | y


def insert_motion(bank, mid, x, y, data, frames, rt=0):
    """add or replace a motion; keeps the directory sorted by key (the game binary-searches
    it, sub_823941F8) with any trailing sentinel (id 0x7fff / 16000) left where it sorts."""
    for e in bank.entries:
        if e['id'] == mid and e['x'] == x and e['y'] == y:
            e.update(data=data, frames=frames, share=False, tail_off=None)
            return e
    ne = dict(y=y, x=x, id=mid, frames=frames, rt=rt, data=data, share=False, tail_off=None)
    k = key_of(mid, x, y)
    i = 0
    while i < len(bank.entries) and key_of(bank.entries[i]['id'], bank.entries[i]['x'], bank.entries[i]['y']) < k:
        i += 1
    # an inserted entry must not inherit 'share' from its new neighbour
    if i < len(bank.entries) and bank.entries[i].get('share'):
        bank.entries[i]['share'] = False
    bank.entries.insert(i, ne)
    return ne


def build_motion(keys, nframes, packed=False):
    """keys: list of dict(frame, count, root (0.01 units, 3 floats), bones {k: {axis: s16}},
    events (optional list of event byte strings, each T[op] long)).
    A key at frame f with count c is the pose the body reaches at frame f+c+1
    (blend over (c+1)*2 ticks). Returns the motion data (frame stream)."""
    recs = []
    for k in sorted(keys, key=lambda k: k['frame']):
        ev = b''.join(k.get('events', []))
        if k.get('bones') is None and k.get('root') is None:
            pose = b''
        elif packed and max(abs(v) for v in k['root']) >= 2 << (PK_ROOT_BITS - 1):
            # (past the packed root's range - +-4096, a dive's drop to 7000: the bones without a root, then the
            # s16 root on its own, as the stock packed copies do - Diving Double Axe Handle 3538's last keys)
            pose = encode_packed({'subs': [{'form': 'mask', 'count': k['count'], 'mask': 0x1FFFFF, 'bones': k['bones']},
                                           {'form': 'root', 'count': k['count'],
                                            'root': tuple(int(v) for v in k['root'])}], 'trail': b''})
        elif packed and k['count'] >= 0x80:
            # (a blend of 128 frames or more - SvR 2008's The 619 holds a pose that long: the full form keeps 7 bits of
            # count, the mask form 8 - every bone and the root as a mask; past 255, 255)
            pose = encode_packed({'subs': [{'form': 'mask', 'count': min(k['count'], 0xff), 'mask': 0x1FFFFF,
                                            'root': k['root'], 'bones': k['bones']}], 'trail': b''})
        elif packed:
            pose = encode_packed({'subs': [{'form': 'full', 'count': k['count'], 'root': k['root'],
                                            'bones': k['bones']}], 'trail': b''})
        else:
            pose = full_pose(k['count'], k['root'], k['bones'])
        recs.append((k['frame'], join_record(ev, pose, packed)))
    return build_stream(make_stream(recs, nframes))


def body_keys(data, T=None, packed=False):
    """decode a character motion into absolute keys (state carried across partial
    sub-records): list of dict(frame, count, root, bones, events)"""
    st = parse_stream(data)
    root = (0.0, 0.0, 0.0)
    bones = {k: {} for k in range(21)}
    out = []
    for f, r in st.records():
        ev, pose = split_record(r, packed)
        evs = [b for op, b in parse_events(ev, T)[0]] if (ev and T is not None) else ([ev] if ev else [])
        cnt = None
        if pose:
            d = decode_packed(pose) if packed else decode_body(pose)
            for s in d['subs']:
                if 'root' in s and s.get('op') not in (0x7b,):
                    root = tuple(s['root'])
                for k, v in s.get('bones', {}).items():
                    if k < 21:
                        bones[k] = dict(v)
                cnt = s.get('count', cnt)
        out.append(dict(frame=f, count=cnt if cnt is not None else 0, root=root,
                        bones={k: dict(v) for k, v in bones.items()}, events=evs,
                        has_pose=bool(pose)))
    return out
