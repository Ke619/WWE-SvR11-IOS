"""HCTP room gimmick props -> JBOY prop models + placements + their solid collision.

Reads the room's placements (50100/10000) and GM.PAC GMMC_GOBJ tags (room 30 table)
and returns, for every placed gimmick object, the models it shows at rest
(param[0] and extra model params), placed at the record's position/ry, and the
collision blobs that are solid at rest.

placement -> 2011 (after svr08_stage's --rotate 180 --offset dx,dz):
  pos' = (-x + dx, y, -z + dz); locator ry' = -ry + pi (2011 turns objects the
  other way; a half turn adds pi).
"""
import math
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # (its own folder: the embeddable Python doesn't add it)
import jboy  # noqa: E402
import svr08_stage as s8  # noqa: E402

import hctp2jboy  # noqa: E402
import yobj  # noqa: E402

GOBJ = os.path.join(os.environ.get('HCTP_UNPACKED', ''), 'gm', 'GMMC_GOBJ')

# class kind -> (model params, solid collision params at rest) by param index (param[0] = model)
# (indices into the tag's float list)
KINDS = {
    44: ([0, 4], []),         # CGmShutter: 1236 syattar, p3 1265 syattar2 (the closing one); collision is room geometry
    89: ([0], [2]),           # gmAttackObjAndWeapon: model, coll p1 (param 2)
    47: ([0], [1]),           # CGmWireNetting: ami, wall coll 2063
    51: ([0], [1]),           # CGmThrowObject: model, coll
    36: ([0], [8]),           # CGmShutObject: kizai_box, p8 its solid box (p1 is the floor zone in front, flat)
    37: ([0], []),            # CGmSwitch: panel (2056 = zone)
    39: ([0], []),            # CGmSwitchOnOffParticle: valve (2057 = zone)
    46: ([0], [1]),           # CGmComeOffObject: roof half, panel coll 2060
    # 49 CGmSwitchLight (button lamp): a light cone drawn with HCTP's light shading;
    # as a plain 2011 object it shows as a grey slab - left out
    # CGmCrateAndWinch: crates, cart (p1 goo_shd, the shadow, left out: drawn by
    # 2011 as a solid grey plank); moving coll
    54: ([0, 1], [3]),
    # -- rooms 31-36 (solid collisions only: flat HMDs are zones, 2030 / 2095 small hit cubes);
    # moving ones (CGmMoveCar 73, GmCarMany 12, GmHeli 20) start outside the rooms: hctp_room moves them
    70: ([1, 2, 3, 4, 5, 6], [7]),   # CGmBrokenCar: body 1 / 2, tyre, door, bonnet, window; coll the body
    81: ([0, 3], [2, 6]),            # CVehicleForkLift: forklift, crate on the forks (shadows 1334/1336 out)
    97: ([0], [1]),                  # CGmThrowObjectHIdden: drum can
    71: ([0], [1]),                  # CGmThrowObjectRepeat: equipment box
    61: ([0, 1], []),                # GmLocker: the two doors
    68: ([2], []),                   # gmToilet: the door
    69: ([0], []),                   # gmSandBag (hangs; shadow 1282 out)
    66: ([6, 9], []),                # gmShower: door, glass
    65: ([0], []),                   # gmTrngMch: running machine
    75: ([0, 1, 13, 14], [2]),       # gmBrokenWall: whole, broken, falling piece, cracked (hctp_room hides 3); coll
    76: ([0, 1], [9]),               # gmTVClash: TV, stand
    77: ([0], []),                   # gmRackWithObj: shelf
    84: ([0], []),                   # gmAttackObjFade: shelf things
    85: ([0], []),                   # gmAttackObjWeapon: bat, chair, pipe stand
    62: ([4], [5]),                  # GmShowWindow: shop window
    60: ([0], []),                   # (room 33) neon sign
    25: ([0], [1]),                  # CGmRion: lion statue
    27: ([0], []),                   # (rooms 35/36) sand
    29: ([2, 3], [1]),               # CGmFountain: water, basin
    24: ([1, 3], [8]),               # CGmTower: top, base (2 / 4: their broken copies)
    # no model: places hctp_room needs (zones in their params)
    72: ([], []),                    # CGmPowerSupply: the generator's hit plane
    98: ([], []),                    # gmHammerThrowAtk: whip into the trailer side
    86: ([], []),                    # CForkliftBoxSupply: the crate reload zone
    74: ([], []),                    # CGmMoveCar path points
    73: ([1, 2], []),                # CGmMoveCar: the two traffic cars (hctp_room drives them)
    96: ([], []),                    # GmFountainThrow: whip into the fountain (centre in params 1, 2)
    12: ([0], []),                   # GmCarMany (room 33): a taxi (hctp_room drives it to its anchor)
    13: ([], []),                    # its anchor (the lane's end)
    20: ([0, 9], []),                # GmHeli: body, propeller (its flight path is its own motion, act 10)
}


def gobj_tags(s):
    hdr = open(os.path.join(GOBJ, f'{10 + s:05d}.bin'), 'rb').read()
    ntag, nfl = struct.unpack_from('<II', hdr, 4)
    tb = open(os.path.join(GOBJ, f'{20 + s:05d}.bin'), 'rb').read()
    fl = np.frombuffer(open(os.path.join(GOBJ, f'{30 + s:05d}.bin'), 'rb').read(), '<f4')
    out = {}
    for i in range(ntag):
        tid, kind, n, first = struct.unpack_from('<4h', tb, 8 * i)
        out[tid] = (kind, [float(x) for x in fl[first:first + n]])
    return out


def placements(stage):
    d = open(os.path.join(stage, '50100', '10000.bin'), 'rb').read()
    n = struct.unpack_from('<I', d, 0)[0]
    out = []
    for i in range(n):
        x, y, z = struct.unpack_from('<3f', d, 0x10 + 0x20 * i)
        t = struct.unpack_from('<h', d, 0x10 + 0x20 * i + 12)[0] & 0x7FFF
        ry = struct.unpack_from('<f', d, 0x10 + 0x20 * i + 20)[0]
        out.append((i, t, (x, y, z), ry))
    return out


def props(stage, room=30):
    """-> list of dict(id, kind, model, pos, ry, colls[ids])"""
    tags = gobj_tags(room - 29)
    out = []
    for i, t, pos, ry in placements(stage):
        if not (100 <= t < 1000) or t not in tags:
            continue
        kind, p = tags[t]
        if kind not in KINDS:
            continue
        mi, ci = KINDS[kind]
        models = [int(p[k]) for k in mi if k < len(p) and p[k] >= 1000]
        colls = [int(p[k]) for k in ci if k < len(p) and 2000 <= p[k] < 3000]
        out.append(dict(rec=i, id=t, kind=kind, models=models, pos=pos, ry=ry, colls=colls))
    return out


def rot_y(ry):
    c, s = math.cos(ry), math.sin(ry)
    return np.array([[c, 0, -s], [0, 1, 0], [s, 0, c]])   # row vectors: w = l @ R + p


PROP_ATTR0 = 0xA0   # prop i's wall triangles get low byte 0xA0 + i (the touched-attribute byte names the prop)


def tag_attr(r, i):
    """r: HMD record (24 floats). Wall triangles of prop i get low byte 0xA0+i (top byte cleared);
    floors (low byte 0, e.g. the roof's 0x200) and climb faces (0xDF) keep theirs."""
    w = struct.unpack('<I', struct.pack('<f', r[23]))[0]
    low = w & 0xFF
    if low in (0x00, 0xDF, 0xDB):
        return r
    nw = (w & 0x00FF00) | (PROP_ATTR0 + i)
    r = r.copy()
    r[23] = struct.unpack('<f', struct.pack('<I', nw))[0]
    return r


def world_collision_lists(stage, plist):
    """per prop: its collision records at rest, HCTP world space, tagged."""
    out = []
    for i, pr in enumerate(plist):
        R = rot_y(pr['ry'])
        recs = []
        for c in pr['colls']:
            f = os.path.join(stage, '50101', f'{c:05d}.hmd')
            if not os.path.exists(f):
                continue
            for r in s8.hmd_read(open(f, 'rb').read()):
                recs.append(tag_attr(s8.hmd_move(r, R, np.array(pr['pos'])), i))
        out.append(recs)
    return out


def world_collision(stage, plist):
    return [r for lst in world_collision_lists(stage, plist) for r in lst]


def to_2011(pos, ry, dx, dz, rotate=180):
    x, y, z = pos
    if rotate == 180:
        x, z = -x, -z
        a = -ry + math.pi
    else:
        a = -ry
    a = (a + math.pi) % (2 * math.pi) - math.pi
    return (x + dx, y, z + dz), a


def build_models(stage, plist, tmpl, outdir, tone=1.0):
    """converts each used prop model (nodes baked, local space) to JBOY files.
    -> {model id: (path, textures)}"""
    os.makedirs(outdir, exist_ok=True)
    done = {}
    for pr in plist:
        for mid in pr['models']:
            if mid in done:
                continue
            M = yobj.read(open(os.path.join(stage, '50101', f'{mid:05d}.yobj'), 'rb').read())
            J = hctp2jboy.convert(M, tmpl, tone=tone, double_sided=True)
            path = os.path.join(outdir, f'{mid}.jboy')
            open(path, 'wb').write(jboy.write(J))
            done[mid] = (path, J.textures)
    return done
