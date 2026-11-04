"""An HCTP room's props + climb spots + converted motions -> GMGB package (78R<n>, or the world's) for the row mod.
Room 30 by default; others by HCTP_ROOM / HCTP_DX / HCTP_DZ / HCTP_OUT (the mod folder)."""
import math
import os
import struct
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # (its own folder: the embeddable Python doesn't add it)
import svr08_stage as s8
import hctp_props, hctp_gimmick, hctp_fake56, hctp2jboy, yobj

ROOM = int(os.environ.get('HCTP_ROOM', '30'))
if not os.environ.get('HCTP_UNPACKED') or not os.environ.get('SVR11_PAC'):
    raise SystemExit('set HCTP_UNPACKED (the unpacked HCTP disc) and SVR11_PAC (the SvR 2011 pac folder)')
UNPACKED = os.environ['HCTP_UNPACKED']
STAGE = os.path.join(UNPACKED, 'bg', 'stg_00%02d' % ROOM)
OUT = os.environ.get('HCTP_OUT', 'mod_row')
PAC11 = os.environ['SVR11_PAC']
BG78 = os.path.join(PAC11, 'bg', 'bg78.pac')
GM = os.path.join(PAC11, 'gm.pac')
BANK = os.environ.get('HCTP_BANK', 'out/mot/bank_h3.bin')
DX, DZ = float(os.environ.get('HCTP_DX', '180')), float(os.environ.get('HCTP_DZ', '-440'))
# (one room of the world stage, build_world.py: its object acts' first key, its first GMPD record, its first
# triangle in HMD 1004, a prefix for its texture names; HCTP_SPEC_OUT: the gimmick parts to a file, not built)
OBJ_ACT = int(os.environ.get('HCTP_OBJ_ACT', '12700'))
REC_BASE = int(os.environ.get('HCTP_REC_BASE', '100'))
TRI_BASE = int(os.environ.get('HCTP_TRI_BASE', '0'))
SPEC_OUT = os.environ.get('HCTP_SPEC_OUT')
TEX_PREFIX = os.environ.get('HCTP_TEX_PREFIX', '')
CLIMB_KEYS = {0xFB: 12541, 0xFD: 12542, 0xF4: 12503, 0xDF: 12503}
# props' rest poses (HCTP: idle acts, or the first frame of their fall / open act)
REST = {1243: 12532, 1234: 12523, 1264: 12531, 1232: 12558, 1252: 12556, 1236: 12555, 1233: 12559}
STILL = {1262, 1235}
RUN_KEYS = {0xFB: 12504, 0xFD: 12544, 0xF4: 12503, 0xDF: 12503}


def to2011(p):
    return np.array([-p[0] + DX, p[1], -p[2] + DZ])


def climb_spots(hmd_bytes):
    """T22 walk-into-wall spots, one per climbable face (pairs of triangles in one plane)."""
    n = struct.unpack_from('<I', hmd_bytes, 4)[0]
    faces = {}
    for k in range(n):
        attr = struct.unpack_from('<I', hmd_bytes, 8 + 96 * k + 92)[0] & 0xFF
        if attr not in CLIMB_KEYS:
            continue
        r = np.array(struct.unpack_from('<24f', hmd_bytes, 8 + 96 * k))
        C = np.array(s8.tri_corners(r))
        nrm = np.cross(C[1] - C[0], C[2] - C[0])
        nrm /= np.linalg.norm(nrm)
        key = (attr, tuple(np.round(nrm, 2)), round(float(np.dot(nrm, C[0])), 1))
        faces.setdefault(key, []).append(C)
    out = []
    for (attr, nrm, _), tris in faces.items():
        P = np.concatenate(tris)
        c = (P.min(0) + P.max(0)) / 2
        w = P.max(0) - P.min(0)
        bottom = P[:, 1].max()                       # y down: the face's foot
        pos = to2011((c[0], bottom, c[2]))
        f = np.array([-nrm[0], -nrm[2]])            # the normal turned by the half turn: (-nx, -nz)
        ry = math.atan2(f[0], f[1]) + math.pi     # (2011 faces the spot's ry + pi: a climb plays toward the wall)
        ry = (ry + math.pi) % (2 * math.pi) - math.pi
        out.append(dict(type=22, words=[0, max(9.0, float(max(w[0], w[2])) / 2 + 2), CLIMB_KEYS[attr], 0],
                        pos=tuple(float(v) for v in pos), ry=ry, attr=attr))
        out.append(dict(type=21, words=[0, out[-1]['words'][1], RUN_KEYS[attr], 2, 99, 0, 0, 0],
                        pos=out[-1]['pos'], ry=ry, attr=attr))
    return out


_, _, _, _, e78 = s8.stage(BG78)
tmpl = hctp2jboy.template({i: s8.unpack(b) for i, b in e78})
plist = hctp_props.props(STAGE, ROOM)
models = {}
import os
os.makedirs('out/props', exist_ok=True)
import jboy
for pr in plist:
    for mid in pr['models']:
        if mid in models:
            continue
        M = yobj.read(open(f'{STAGE}/50101/{mid:05d}.yobj', 'rb').read())
        # (a model that never plays a key is drawn with its nodes' own
        # transforms: those that don't move get none - 1262 the cart body,
        # 1235 the button panel)
        J = hctp2jboy.convert(M, tmpl, keep_nodes=mid not in STILL, double_sided=True)
        path = f'out/props/{mid}.jboy'
        open(path, 'wb').write(jboy.write(J))
        models[mid] = (path, J.textures)
# steam jets (HCTP particles 32 / 34: model 1237 zyouki puffs, hit volume along local +z):
# a few zyouki quads per jet, moved along it by hctp_room while the valve has it on
STEAM = {32: (210, 2058), 34: (215, 2068)}   # particle -> (valve, hit volume)
PUFFS = 5
steam_lines, puffs = [], []
if ROOM == 30:
    M = yobj.read(open(f'{STAGE}/50101/01237.yobj', 'rb').read())
    J = hctp2jboy.convert(M, tmpl, keep_nodes=False, double_sided=True)
    if os.environ.get('HCTP_PUFF_TEX'):   # (debug: the puff with another texture)
        J.textures = [os.environ['HCTP_PUFF_TEX']]
    open('out/props/1237.jboy', 'wb').write(jboy.write(J))
    models[1237] = ('out/props/1237.jboy', J.textures)
for _, t, ppos, pry in hctp_props.placements(STAGE):
    if ROOM != 30 or t not in STEAM:
        continue
    valve, coll = STEAM[t]
    R = [np.array(r) for r in s8.hmd_read(open(f'{STAGE}/50101/{coll:05d}.hmd', 'rb').read())]
    C = np.array([c for r in R for c in s8.tri_corners(r)])
    o = to2011(ppos)
    d = (-math.sin(pry), -math.cos(pry))           # local +z, mirrored into 2011
    steam_lines.append(f"steam {t} {valve} {o[0]:.2f} {o[1]:.2f} {o[2]:.2f} {d[0]:.4f} {d[1]:.4f} "
                       f"{C[:, 2].max():.1f} {C[:, 0].max():.1f} {C[:, 1].max():.1f}")
    for k in range(PUFFS):
        puffs.append((t, k, o))
tb = hctp_fake56.texbank(STAGE)
tex = {}
for mid, (path, tl) in models.items():
    for t in tl:
        if t not in tex:
            tex[t] = hctp_fake56.dds(tb[t], hctp_fake56.alpha_kind(tb[t]))
# the steam puff: 2011 props draw with an alpha cut-off, which drops all of
# the soft zyouki smoke (alpha < 128 almost everywhere) - its alpha becomes
# an ordered dither (a texel is solid as often as it was opaque), white-ish
if 'zyouki' in tb:
    z = tb['zyouki'].astype(np.float32)
    h0, w0 = z.shape[:2]
    big = np.array(Image.fromarray(tb['zyouki'], 'RGBA').resize((w0 * 4, h0 * 4), Image.BILINEAR)).astype(np.float32)
    bayer = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]], np.float32) / 16.0
    thr = np.tile(bayer, (big.shape[0] // 4, big.shape[1] // 4))
    a = np.clip(big[..., 3] / 255.0 * 1.8, 0, 1)
    big[..., 3] = np.where(a > thr, 255, 0)
    big[..., :3] = np.clip(big[..., :3] * 0.5 + 128, 0, 255)
    tex['zyouki'] = hctp_fake56.dds(big.astype(np.uint8), 'cut')
spec = []
for pr in plist:
    pos, a = hctp_props.to_2011(pr['pos'], pr['ry'], DX, DZ)
    for mid in pr['models']:
        idle = REST.get(mid, 0)
        if ROOM != 30:   # rooms 31-36: a model's rest act from its tag (kind: (model param, act param))
            prm = hctp_props.gobj_tags(ROOM - 29)[pr['id']][1]
            ix = {71: (0, 8), 81: (3, 4)}.get(pr['kind'])
            if ix and int(prm[ix[0]]) == mid and prm[ix[1]] > 0:
                idle = OBJ_ACT + int(prm[ix[1]])
        # (the equipment crate's model turned half round: its lid stood on the side it is whipped in from -
        # its solid box is symmetric, and its floor zone stays where HCTP has it)
        ry = a + math.pi if pr['kind'] == 36 else a
        spec.append(dict(model=models[mid][0], pos=pos, ry=ry, note=f"{pr['id']} m{mid}", idle=idle))
# climb spots from the room collision with props (HCTP coordinates, original attributes)
hmd = open(STAGE + r'\00999.hmd', 'rb').read()
extra = hctp_props.world_collision(STAGE, plist)
hmd = s8.hmd_write(s8.hmd_read(hmd) + extra)
spots = climb_spots(hmd)
for s in spots:
    print(f"T{s['type']} attr {s['attr']:#x} key {s['words'][2]} r {s['words'][1]:.1f} at "
          f"({s['pos'][0]:.1f}, {s['pos'][1]:.1f}, {s['pos'][2]:.1f}) ry {s['ry']:.2f}")
bank = open(BANK, 'rb').read()
# (the puffs first: objects past the first 20 weren't drawn)
spec = [dict(model=models[1237][0], pos=tuple(float(v) for v in o), ry=0.0, note=f'steam {t} puff {k}')
        for t, k, o in puffs] + spec
os.makedirs(OUT, exist_ok=True)
if SPEC_OUT:
    import pickle
    if TEX_PREFIX:   # (rooms reuse texture names: each room's get its prefix)
        ren = {t: (TEX_PREFIX + t)[:15] for t in tex}
        if len(set(ren.values())) != len(ren):
            raise SystemExit('texture names clash after the prefix')
        for path in {p['model'] for p in spec}:
            J = jboy.read(open(path, 'rb').read())
            J.textures = [ren.get(t, t) for t in J.textures]
            open(path, 'wb').write(jboy.write(J))
        tex = {ren[t]: d for t, d in tex.items()}
    for p in spec:
        p['model'] = os.path.abspath(p['model'])
    pickle.dump(dict(spec=spec, spots=spots, tex=tex), open(SPEC_OUT, 'wb'))
else:
    hctp_gimmick.build(GM, f'{OUT}/gimmick.pac', entry=os.environ.get('HCTP_ENTRY', '78R%d' % (ROOM - 30)), props=spec, textures=tex, report=True,
                       records=spots, bank=bank, keep_models=False)


# ------------------------------------------------------------ hctp.txt (hctp_room.cpp's room data)
# One line per item, space separated, 2011 world coordinates (y down), yaw in radians:
#   prop <name> <hctp id> <kind> <attr byte> <x> <y> <z> <ry> recs=<record ids,...> tris=<HMD 1004 indices,...>
#   weapon <hctp id> <model> <x> <y> <z>         (where a stack's weapon appears)
#   key <key> <frames>                           (converted motions hctp_room plays)
KIND = {89: 'stack', 51: 'throw', 47: 'wall', 46: 'roof', 37: 'switch', 39: 'valve', 44: 'shutter',
        54: 'cart', 49: 'lamp', 36: 'crate',
        # rooms 31-36 (re_rooms31_36.md)
        70: 'car', 72: 'power', 73: 'movecar', 74: 'path', 98: 'hammer', 81: 'forklift', 86: 'boxsupply',
        97: 'drums', 71: 'repeat', 61: 'locker', 68: 'toilet', 69: 'sandbag', 66: 'shower', 65: 'treadmill',
        75: 'brokenwall', 76: 'tv', 77: 'rack', 84: 'fade', 85: 'rackweapon', 62: 'window', 60: 'neon',
        25: 'lion', 27: 'sand', 29: 'fountain', 24: 'tower', 96: 'fountainthrow', 12: 'traffic', 13: 'anchor',
        20: 'heli'}
# HCTP weapon -> 2011 (WPBS): cardboard box 56 -> 17 (78_obj_cbox), barrel 57 -> trash can 63,
# lead pipe 148 -> kendo stick 88 (no pipe in 2011), trash can 9 -> 63
WEAPON_2011 = {56: 17, 57: 63, 148: 88, 9: 63}
tags = hctp_props.gobj_tags(ROOM - 29)


def final_index_map(n):
    """HMD record k -> index after stage_wrap's dummies (no index may end in 0xFF)."""
    out, m = 0, []
    for k in range(n):
        while (out & 0xFF) == 0xFF:
            out += 1
        m.append(out)
        out += 1
    return m


n0 = struct.unpack_from('<I', open(STAGE + r'\00999.hmd', 'rb').read(), 4)[0]
lists = hctp_props.world_collision_lists(STAGE, plist)
fmap = final_index_map(TRI_BASE + n0 + sum(len(x) for x in lists))[TRI_BASE:]
lines, at, rid = [], n0, REC_BASE + len(puffs)
weapon_lines = []
# weapons the room's objects become (HCTP weapon -> 2011): bat 7 / lead pipe 148 -> kendo 88, chair 4
HCTP_WEAPON_OBJ = {7: 88, 148: 88, 4: 4}
for i, pr in enumerate(plist):
    kind = KIND.get(pr['kind'], f"k{pr['kind']}")
    recs = list(range(rid, rid + len(pr['models'])))
    rid += len(pr['models'])
    tris = [fmap[k] for k in range(at, at + len(lists[i]))]
    at += len(lists[i])
    pos, a = hctp_props.to_2011(pr['pos'], pr['ry'], DX, DZ)
    box = ''
    if lists[i]:                   # the collision's 2011 footprint: x0,x1,y0,y1,z0,z1
        P = np.array([to2011(c) for r in lists[i] for c in s8.tri_corners(np.array(r))])
        lo, hi = P.min(0), P.max(0)
        box = f" box={lo[0]:.1f},{hi[0]:.1f},{lo[1]:.1f},{hi[1]:.1f},{lo[2]:.1f},{hi[2]:.1f}"
    if kind in ('switch', 'valve'):  # the press zone (CGmSwitch p0 / valve p0: an HMD shape, not solid)
        zf = f"{STAGE}/50101/{int(tags[pr['id']][1][1]):05d}.hmd"
        if os.path.exists(zf):
            R = hctp_props.rot_y(pr['ry'])
            P = np.array([to2011(c) for r in s8.hmd_read(open(zf, 'rb').read())
                          for c in s8.tri_corners(np.array(s8.hmd_move(r, R, np.array(pr['pos']))))])
            lo, hi = P.min(0), P.max(0)
            box += f" zone={lo[0]:.1f},{hi[0]:.1f},{lo[1]:.1f},{hi[1]:.1f},{lo[2]:.1f},{hi[2]:.1f}"
    # rooms 31-36: the tag's params (p=), the record models in order (m=) and every HMD shape the params
    # name as its 2011 box (h<id>=x0,x1,y0,y1,z0,z1: zones and alternate collisions)
    extra = ''
    if ROOM != 30:
        prm = tags[pr['id']][1]
        extra = f" p={','.join('%g' % v for v in prm)} m={','.join(map(str, pr['models']))}"
        R = hctp_props.rot_y(pr['ry'])
        for v in prm:
            hf = f"{STAGE}/50101/{int(v):05d}.hmd"
            if 2000 <= v < 3000 and v == int(v) and os.path.exists(hf):
                P = np.array([to2011(c) for r in s8.hmd_read(open(hf, 'rb').read())
                              for c in s8.tri_corners(np.array(s8.hmd_move(r, R, np.array(pr['pos']))))])
                lo, hi = P.min(0), P.max(0)
                extra += f" h{int(v)}={lo[0]:.1f},{hi[0]:.1f},{lo[1]:.1f},{hi[1]:.1f},{lo[2]:.1f},{hi[2]:.1f}"
    if kind == 'rackweapon':   # gmAttackObjWeapon: the shelf item becomes weapon p5 (offsets p9..p11)
        prm = tags[pr['id']][1]
        if int(prm[5]) in HCTP_WEAPON_OBJ:
            p2, _ = hctp_props.to_2011((pr['pos'][0] + prm[9], pr['pos'][1] + prm[10], pr['pos'][2] + prm[11]), 0.0, DX, DZ)
            weapon_lines.append(f"weapon {pr['id']} {HCTP_WEAPON_OBJ[int(prm[5])]} {p2[0]:.2f} 0.00 {p2[2]:.2f}")  # (on the floor)
    if kind == 'fountainthrow':   # GmFountainThrow: the basin centre (params 1, 2), radius sqrt(600)
        prm = tags[pr['id']][1]
        c = to2011((prm[1], 0.0, prm[2]))
        box += f" zone={c[0] - 24.5:.1f},{c[0] + 24.5:.1f},0,0,{c[2] - 24.5:.1f},{c[2] + 24.5:.1f}"
    if kind == 'hammer':   # gmHammerThrowAtk: its trigger box, params x 1..2 and z 5..6 (room space)
        prm = tags[pr['id']][1]
        P = np.array([to2011((xx, 0.0, zz)) for xx in prm[1:3] for zz in prm[5:7]])
        lo, hi = P.min(0), P.max(0)
        box += f" zone={lo[0]:.1f},{hi[0]:.1f},0,0,{lo[2]:.1f},{hi[2]:.1f}"
    lines.append(f"prop {pr['id']}_{i} {pr['id']} {kind} {0xA0 + i} {pos[0]:.2f} {pos[1]:.2f} {pos[2]:.2f} {a:.4f} "
                 f"recs={','.join(map(str, recs))} tris={','.join(map(str, tris))}{box}{extra}")
    t = tags[pr['id']][1]
    if kind == 'stack':            # gmAttackObjAndWeapon: p4 (param 5) = the GmWeaponPosRot tag
        ws = tags.get(int(t[5]))
        if ws:
            w = ws[1]              # [0, weapon, act, 0, 0, x, y, z, ...]; upper-level y is stored +30
            wy = -abs(w[6])
            p2, _ = hctp_props.to_2011((w[5], wy, w[7]), 0.0, DX, DZ)
            lines.append(f"weapon {pr['id']} {WEAPON_2011.get(int(w[1]), 63)} {p2[0]:.2f} {p2[1]:.2f} {p2[2]:.2f}")
    if kind == 'throw' and len(t) > 9 and t[9] > 0:   # CGmThrowObject p8 (param 9) = weapon after p10 frames
        lines.append(f"weapon {pr['id']} {WEAPON_2011.get(int(t[9]), 88)} {pos[0]:.2f} {pos[1]:.2f} {pos[2]:.2f}")
import svr08_gimmick as G
T, ents, _ = G.bank_parse(bank)
frames = {}
for e in ents:
    if 12500 <= e[2] < 14000:
        frames[e[2]] = max(frames.get(e[2], 0), e[4])
# The helicopter's flight (GmHeli: room act 10 on its body - its root keys are its path, 0.01 units, room
# space): heli <hctp frame> <x> <y> <z> <yaw> lines, 2011 space
if any(pr['kind'] == 20 for pr in plist):
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'mot'))
    import hmot, objpose, fk
    _, hents = hmot.read_bank(open(os.path.join(STAGE, '50102.bin'), 'rb').read())
    he = next(e for e in hents if e['id'] == 10 and e.get('y', 0) == 0)
    for fr, rot, tr, root in objpose.states(he['data']):
        q = to2011(tuple(v * 0.01 for v in root))
        ry = rot.get(0, (0, 0, 0))[1] * fk.ANG
        weapon_lines.append(f"heli {fr} {q[0]:.2f} {q[1]:.2f} {q[2]:.2f} {(-ry + math.pi):.4f}")
# Times Square's climbable poles (GmTimesPole: data rows of kind 40, params 1-3 = the pole top)
for tid, (kd, prm) in sorted(tags.items()):
    if kd == 40 and len(prm) >= 4:
        q = to2011((prm[1], prm[2], prm[3]))
        weapon_lines.append(f"pole {tid} {q[0]:.2f} {q[1]:.2f} {q[2]:.2f}")
lines += weapon_lines
lines += steam_lines
for k, (t, j, o) in enumerate(puffs):  # (the first records, in spec order)
    lines.append(f"prop steam{t}_{j} {t} puff 0 {o[0]:.2f} {o[1]:.2f} {o[2]:.2f} 0 recs={REC_BASE + k} tris=")
lines += [f"key {k} {v}" for k, v in sorted(frames.items())]
open(f'{OUT}/hctp.txt', 'w').write('\n'.join(lines) + '\n')
print(f'hctp.txt: {len(lines)} lines')


# ------------------------------------------------------------ weapons.txt (the manifest's weapons= line)
# The weapons lying in the room at the bell: HCTP's 50000.txt 'w' spots {id, 0, (x, y, z), (rx, ry, rz), 0, w}
# and its GmWeaponPosRot placements (kind 95: p1 weapon, p5..p7 position, upper-level y stored +30), as
# 2011 weapons; 'none' when nothing maps (else 2011's parking weapons would lie in the room).
# HCTP weapon ids (M.PAC WPON_PROP: model = id x 10) -> 2011 (WPBS)
HCTP_WEAPON = {3: 63, 9: 63, 8: 63, 13: 65, 4: 4, 7: 88, 102: 88, 148: 88, 150: 88, 56: 17, 57: 63}
import re
spots = []
for line in open(os.path.join(STAGE, '50000.txt'), 'rb').read().decode('shift_jis', 'replace').splitlines():
    m = re.match(r'\s*\{\s*(\d+)\s*,\s*\d+\s*,\s*\(([^)]*)\)\s*,\s*\(([^)]*)\)\s*,[^,]*,\s*@?w\s*\}', line.split('//')[0])
    if m:
        p = [float(v) for v in m.group(2).split(',')]
        r = [float(v) for v in m.group(3).split(',')]
        spots.append((int(m.group(1)), p, r[1]))
for _, t, ppos, pry in hctp_props.placements(STAGE):
    tg = tags.get(t)
    if tg and tg[0] == 95:
        w = tg[1]
        spots.append((int(w[1]), [w[5], -abs(w[6]), w[7]], math.degrees(pry)))
wl = []
for wid, p, ry in spots:
    if wid not in HCTP_WEAPON:
        print(f'weapon {wid} at {p}: no 2011 weapon, left out')
        continue
    q, _ = hctp_props.to_2011(tuple(p), 0.0, DX, DZ)
    wl.append(f'{HCTP_WEAPON[wid]},{q[0]:.1f},{q[1]:.1f},{q[2]:.1f},{(180.0 - ry) % 360:.0f}')
open(f'{OUT}/weapons.txt', 'w').write(';'.join(wl) if wl else 'none')
print(f'weapons: {len(wl)} of {len(spots)}')
