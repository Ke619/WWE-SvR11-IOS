"""HCTP (PS2) CEKey motions -> SvR2011 YMKs motions (prototype).

    python hctp2ymks.py <hctp bank.bin> <id[=newid],...> [--into <2011 YMKs bank>] [--out <bank.bin>]
                        [--events keep|jumps|none] [--objects] [--compact]
                        [--attach <prop bank>:<act>:<y>[,...]] [--jumpmap hctp=2011,...]
                        [--camera-raw] [--fov <rad>]

  <hctp bank>   e.g. m/MOT_GAME/00004.bin (backstage acts) or bg/stg_0030/50102.bin (room props;
                pass --objects: every track there is a prop track)
  ids           HCTP motion ids, each optionally renamed (12034=12534). Every track of the id
                (all x variants, all y tracks) is converted.
  --into        2011 YMKs bank to add the motions to (e.g. a GMGB package bank, sub-entry 5).
                Without it a new bank is made with 2011's event-size table (78PK's head) and
                the 0x7fff sentinel.
  --events      keep (default): copy every event whose op has the same size in both games
                (all HCTP ops except 185); jump ops 49/50/51 get their ids renamed when the target
                is in the converted set, otherwise they are dropped. jumps: keep only 49/50/51 and
                1 (speed). none: drop all events.
  --jumpmap     ids that jump events (ops 49/50/51) to motions outside the converted set should
                use in 2011 (e.g. 443=<2011 get-up id>); unmapped outside jumps are dropped.
  --attach      add a prop motion from an HCTP object bank (e.g. 50102.bin:33:2 = room-30
                cardboard collapse) as track y of every converted id (2011 GMGB grapple keys
                animate their T23 objects with tracks y2.., T33 p2..p4).
  --compact     keep s16 roots: full keys keep HCTP's 0x60+count / 0x7c form and masked keys
                become 0x75, instead of 2011's stock f32-root 0x7d / 0x50+count / 0x7a (all are
                read by the 2011 decoder sub_82391380; 0x75 is rare in stock data).

Track handling (y = key low byte):
  character tracks (CMotionData, HCTP type table): re-encoded with 2011's m.pac types: kubi/te
     y,z and tsumasaki y widen from 8 to 16 bits (the HCTP byte is the high byte, so the s16
     value is unchanged), ashi gains x = 0. Angles, Euler order, units, the motion bind
     (HCTP rest pose) and the root are identical, so no retargeting is applied.
       full 0x60-0x6f / 0x7c / 0x7d -> 0x7d (110 bytes) [--compact: same op]
       masked 0x50-0x5f / 0x7a (s16 root) -> 0x50+count / 0x7a with an f32 root (2011's stock
           masked forms; 2011's 0x50-0x5f / 0x7a carry f32, HCTP's s16) [--compact: 0x75, s16]
       0x7e / 0x7f / 0x7b / single bone 0x00-0x14 -> same op
  object tracks (CSD3MotionWeapon, every bone x,y,z s16; bones = nodes root, bone01..bone19):
       0x7a (s16 root + mask) -> 0x75, 0x7d (root) -> 0x7e, 0x7e / 0x7b / single bone copied.
  5-byte tracks (y 10-12, 30-32): copied (same [count][s16][s16] keys in 2011).
  camera tracks (y >= 50): HCTP keys are 13 bytes [count][s16 pos x3, 0.01 u][s16 rot x3]
     (= 2011 camera mode 0). 2011 GMGB / m.pac cameras are mode 1 (29 bytes, f32 + fov), so
     each 13-byte key becomes [count][f32 pos][f32 rot rad][f32 fov] with --fov (default 0.7217
     rad = 2011 78PK median; HCTP has no fov in the key). --camera-raw copies the 13-byte keys.
  Frame stream: HCTP escape 0x60 + u8 size -> 2011 0x60 + u16 size (used for sizes >= 0xa0).
"""
import os
import struct
import sys
import collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))  # (tools\: ymks.py)
import hmot  # noqa: E402
import ymks as Y  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
TEMPLATE_BANK = os.path.join(HERE, '78PK_bank.bin')   # 2011 GMGB 78PK motion bank (head = T table)
JUMP_OPS = (49, 50, 51)
FADE_OP = 18                         # (screen fade: r, g, b, alpha 0x80 = full, frames - dropped)


# ---------------------------------------------------------------- tracks

def conv_char_pose(pose, compact=False):
    d = hmot.decode_body(pose)
    if d['trail']:
        raise ValueError('trailing bytes in a character pose')
    subs = []
    for s in d['subs']:
        op = s['op']
        if op in (0x7d, 0x7c) or 0x60 <= op <= 0x6f:
            root = tuple(s['root'])
            if compact:
                n = dict(op=op if op != 0x7d else 0x7d, count=s['count'], root=root, bones=s['bones'])
            else:
                n = dict(op=0x7d, count=s['count'], root=tuple(float(v) for v in root), bones=s['bones'])
        elif op == 0x7a or 0x50 <= op <= 0x5f:
            if compact:
                n = dict(op=0x75, count=s['count'], root=tuple(s['root']), mask=s['mask'], bones=s['bones'])
            else:     # stock 2011 style: 0x50+count (count <= 15) or 0x7a, f32 root
                n = dict(op=0x50 + s['count'] if s['count'] <= 15 else 0x7a, count=s['count'],
                         root=tuple(float(v) for v in s['root']), mask=s['mask'], bones=s['bones'])
        elif op == 0x7b:
            n = dict(op=0x7b, ref=s['ref'], count=s['count'], root=tuple(s['trans']))
        elif op in (0x7e, 0x7f):
            n = dict(op=op, count=s['count'], root=tuple(s['root']))
        else:
            n = dict(op=op, count=s['count'], bones=s['bones'])
        subs.append(n)
    return Y.encode_body({'subs': subs, 'trail': b''}, Y.TYPES_M1)


def conv_obj_pose(pose):
    d = hmot.decode_obj(pose)
    if d['trail']:
        raise ValueError('trailing bytes in an object pose')
    out = bytearray()
    for s in d['subs']:
        op = s['op']
        if op == 0x7a:
            m = s['mask']
            out += bytes([0x75, s['count']]) + struct.pack('<3h', *s['root']) + bytes([m & 0xff, m >> 8 & 0xff, m >> 16 & 0xff])
            for k in range(21):
                if m >> k & 1:
                    b = s['bones'][k]
                    out += struct.pack('<3h', b['x'], b['y'], b['z'])
        elif op in (0x7d, 0x7e):
            out += bytes([0x7e, s['count']]) + struct.pack('<3h', *s['root'])
        elif op == 0x7b:
            out += bytes([0x7b, s['ref'], s['count']]) + struct.pack('<3h', *s['trans'])
        else:
            b = s['bones'][op]
            out += bytes([op, s['count']]) + struct.pack('<3h', b['x'], b['y'], b['z'])
    return bytes(out)


def conv_events(ev, Th, T11, mode, idmap, rep):
    if not ev or mode == 'none':
        if ev:
            rep['events dropped (--events none)'] += 1
        return b''
    lst, ok = Y.parse_events(ev, Th)
    out = bytearray()
    for op, b in lst:
        if op is None:
            rep['event bytes past the size table (dropped)'] += 1
            continue
        if op in JUMP_OPS:
            tgt = struct.unpack_from('<H', b, 1)[0]
            if tgt in idmap:
                b = bytearray(b)
                struct.pack_into('<H', b, 1, idmap[tgt])
                out += b
                rep['jump kept op%d -> %d' % (op, idmap[tgt])] += 1
            else:
                rep['jump dropped op%d (target %d not converted)' % (op, tgt)] += 1
            continue
        # (the screen fade: HCTP's gates' exit acts fade to black for the next stage's load, which fades back
        # in; nothing fades back in 2011 - the screen stayed black)
        if op == FADE_OP:
            rep['event dropped op%d (screen fade)' % op] += 1
            continue
        if mode == 'jumps' and op != 1:
            rep['event dropped op%d (--events jumps)' % op] += 1
            continue
        if T11[op] != len(b):
            rep['event dropped op%d (size %d in HCTP, %d in 2011)' % (op, len(b), T11[op])] += 1
            continue
        out += b
        rep['event kept op%d' % op] += 1
    if len(out) > 0x7f:
        rep['event block > 127 bytes (cut)'] += 1
        out = out[:0x7f]
    return bytes(out)


FOV = 0.7217285633087158


def conv_cam_pose(pose, rep, fov=None):
    if len(pose) != 13:
        rep['camera key of %d bytes copied raw' % len(pose)] += 1
        return pose
    c = pose[0]
    pos = struct.unpack_from('<3h', pose, 1)
    rot = struct.unpack_from('<3h', pose, 7)
    return bytes([c]) + struct.pack('<3f3ff', *[float(v) for v in pos], *[v * Y.ANG for v in rot], FOV if fov is None else fov)


def conv_track(data, kind, Th, T11, events, idmap, rep, compact=False):
    recs = []
    for f, r in hmot.records(data):
        ev, pose = Y.split_record(r)
        ev2 = conv_events(ev, Th, T11, events, idmap, rep)
        if len(pose) <= 1:
            p2 = pose
        elif kind == 'char':
            p2 = conv_char_pose(pose, compact)
        elif kind == 'obj':
            p2 = conv_obj_pose(pose)
        elif kind == 'cam29':
            p2 = conv_cam_pose(pose, rep)
        else:
            p2 = pose
        if not ev2 and not p2 and (ev or pose):
            continue                 # nothing left in this record: the frame becomes a skip
        recs.append((f, Y.join_record(ev2, p2)))
    return Y.build_stream(Y.make_stream(recs, stream_frames(data)))


def stream_frames(data):
    """total frames of an HCTP stream (records + skips, trailing skips included)"""
    p, f = 0, 0
    while p < len(data):
        b = data[p]
        if b < 0x60:
            f += b; p += 1; continue
        sz = data[p + 1] if b == 0x60 else b - 0x60
        p += (2 if b == 0x60 else 1) + sz
        f += 1
    return f


# ---------------------------------------------------------------- bank

def empty_bank():
    B = Y.read_bank(open(TEMPLATE_BANK, 'rb').read())
    B.entries = [e for e in B.entries if e['id'] == 0x7fff]
    for e in B.entries:
        e['share'] = False
    B.lead = b''
    return B


def convert(hbank, ids, into=None, events='keep', objects=False, compact=False, attach=(), jumpmap=None, camera_raw=False, fov=None):
    global FOV
    if fov is not None:
        FOV = fov
    Th, ents = hmot.read_bank(open(hbank, 'rb').read())
    B = Y.read_bank(open(into, 'rb').read()) if into else empty_bank()
    T11 = B.T
    idmap = dict(jumpmap or {})
    idmap.update(dict(ids))
    rep = collections.Counter()
    done = []
    for old, new in ids:
        tracks = [e for e in ents if e['id'] == old]
        if not tracks:
            rep['id %d not in the HCTP bank' % old] += 1
            continue
        for e in tracks:
            if not e['data']:
                rep['empty track copied as empty'] += 1
                Y.insert_motion(B, new, e['x'], e['y'], b'', e['frames'])
                continue
            kind = 'obj' if objects else hmot.track_kind(e['data'], e['y'])
            if kind is None:
                kind = 'raw'
                rep['track of unknown layout copied raw (%d x%d y%d)' % (old, e['x'], e['y'])] += 1
            if e['y'] >= 50 and kind not in ('char', 'obj'):
                kind = 'raw' if camera_raw else 'cam29'
            if kind == 'char' and e['y'] >= 2:
                rep['note: y%d decoded as a character (third wrestler or a prop that parses as one)' % e['y']] += 1
            data = conv_track(e['data'], kind, Th, T11, events, idmap, rep, compact)
            Y.insert_motion(B, new, e['x'], e['y'], data, e['frames'])
            rep['tracks %s' % kind] += 1
            done.append((old, new, e['x'], e['y'], kind, e['frames'], len(e['data']), len(data)))
    for pbank, act, y in attach:
        Tp, pents = hmot.read_bank(open(pbank, 'rb').read())
        pe = [e for e in pents if e['id'] == act]
        if not pe:
            rep['attach: act %d not in %s' % (act, os.path.basename(pbank))] += 1
            continue
        pe = pe[0]
        data = conv_track(pe['data'], 'obj', Tp, T11, events, idmap, rep, compact)
        for old, new in ids:
            Y.insert_motion(B, new, 0, y, data, pe['frames'])
            done.append((act, new, 0, y, 'prop', pe['frames'], len(pe['data']), len(data)))
            rep['attached prop tracks'] += 1
    return B, done, rep


def main():
    a = sys.argv[1:]
    if len(a) < 2:
        print(__doc__)
        return

    def opt(k, d=None):
        return a[a.index(k) + 1] if k in a else d
    ids = []
    for tok in a[1].split(','):
        o, _, n = tok.partition('=')
        ids.append((int(o), int(n or o)))
    att = []
    if opt('--attach'):
        for tok in opt('--attach').split(','):
            p, act, y = tok.rsplit(':', 2)
            att.append((p, int(act), int(y)))
    jm = {}
    if opt('--jumpmap'):
        for tok in opt('--jumpmap').split(','):
            o, n = tok.split('=')
            jm[int(o)] = int(n)
    B, done, rep = convert(a[0], ids, opt('--into'), opt('--events', 'keep'), '--objects' in a, '--compact' in a, att, jm,
                           '--camera-raw' in a, float(opt('--fov')) if opt('--fov') else None)
    out = opt('--out', 'converted_bank.bin')
    open(out, 'wb').write(Y.write_bank(B))
    for d in done:
        print('HCTP %5d -> %5d  x%-3d y%-3d %-5s frames %4d  %6d -> %6d bytes' % d)
    if True:
        print('report:')
        for k, v in sorted(rep.items()):
            print('  %-70s %d' % (k, v))
    print('wrote', out, len(B.entries), 'entries')


if __name__ == '__main__':
    main()
