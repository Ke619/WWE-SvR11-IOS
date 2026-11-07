"""The HCTP backstage as one stage you walk through -> one SvR2011 backstage row mod.

    py build_world.py <HCTP unpacked folder> <2011 pac folder> <work folder> <mod folder> [rooms, e.g. 30,31,32]

The rooms (build_room.py's ROOMS; default 30-34 - backstage_port.py's hctpworld / hctpfountain) in one bg78
(world_stage.py), all in the same place - the parking room's, where one room alone sits (the match camera's
target stays inside the stock backstage: rooms side by side 1000 apart left it looking at nothing). Their props,
climb spots and motions
in one gimmick package (objects 1000-1139, each room's object acts from its own first key); hctp.txt with a
'room' line before each room's lines (its models, objects, object acts, floor, collision triangles and climb-spot
records: hctp_room.cpp keeps only the current room's), its 'door' lines (HCTP's 50000.txt move points and where
HCTP's gates send them, GATES; the exit acts are in the bank from 12900) and 'spawn' lines (its weapons, put down on the first
visit; the first room's are the manifest's weapons=).
"""
import math
import os
import pickle
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, 'mot'))
import build_room as B  # noqa: E402

# (the whole backstage, about 625 stage meshes: a stage over the game's vertex window is placed elsewhere since
# 2.1.5, src/model_mem.cpp - before, ~400 meshes crashed the load)
WORLD_ROOMS = (30, 31, 32, 33, 34)
OBJ_ACT = {31: 13000, 32: 13100, 33: 13200, 34: 13300, 35: 13400, 36: 13500}
FIRST_OBJ, LAST_OBJ = 1000, 1139    # (stock CFRS rooms' object ids; hctp_room shows only the current room's)
TEX_UPSCALE = int(os.environ.get('HCTP_WORLD_UPSCALE', '2'))                    # (props' textures: the stage, the package and their buffers share a ~17 MB window)
# HCTP's own gates (SLES_520.36: CGate4 / CMoveStage, tables g_stg_audi_list and bord_group_list; read by
# tools/hctp/re/gate/gate_tables.py): a move point (50000.txt 'm') leads to <stage>'s move point <entry>, the
# wrestler going through plays <exit act> first. 201 leads back to the arena (no room here). 209 opens with an
# unlock in HCTP (hide item 11): here it is open.
GATES = {201: (0, 200, 15919), 202: (32, 203, 15900), 203: (30, 202, 15907), 204: (31, 205, 15902),
         205: (30, 204, 15902), 206: (34, 207, 15919), 207: (31, 206, 15919), 208: (31, 206, 15919),
         209: (33, 210, 15909), 210: (34, 209, 15909)}
# (Fountain Square, 35, stands in for Outside the Arena, 34: HCTP sends 31's gate 206 to 35's 208 when the match
# started in stage 0x16 / 0x17 - CMoveStage)
GATES_FOUNTAIN = {206: (35, 208, 15919)}
# where the wrestlers stand coming in through a move point: wrestler i at the point + its rotation of
# (OFFX[i], 0, OFFZ[i] - 33) (CMoveStage's Rear: offx / offz); a gate takes a wrestler within 10 of the point
# on each axis (CGate4::Action) who presses the action button walking
OFFX = (-9.0, 9.0, -9.0, 9.0, -12.0, 12.0, 0.0, 14.0)
OFFZ = (8.0, 8.0, -8.0, -8.0, 0.0, 0.0, 0.0, -10.0)
ENTRY_BACK = 33.0
EXIT_ACT_BASE = 12900               # (HCTP's 15900-15919 in the world bank: 12900 + n)
NEEDS_GATES = '2.1.6'               # (the first game with them: hctp_room.cpp's gate lines)


def doors_of(stage):
    """HCTP 50000.txt move points {id, 0, (x, y, z), (rx, ry, rz), act, m} -> [(id, (x, y, z), ry degrees)]"""
    out = []
    for line in open(os.path.join(stage, '50000.txt'), 'rb').read().decode('shift_jis', 'replace').splitlines():
        m = re.match(r'\s*\{\s*(\d+)\s*,\s*\d+\s*,\s*\(([^)]*)\)\s*,\s*\(([^)]*)\)\s*,[^,]*,\s*@?m\s*\}', line.split('//')[0])
        if m:
            out.append((int(m.group(1)), tuple(float(v) for v in m.group(2).split(',')),
                        float(m.group(3).split(',')[1])))
    return out


def gate_line(did, to, p, ry, dx, dz):
    """a hctp.txt 'door' line: id, the move point it leads to, its place (the trigger), its exit act and the way
    out (yaw), then where wrestlers coming in through it stand (8 'x,z' spots: HCTP's offsets turned by the
    point's y rotation) and the way in. The world has HCTP's x and z turned round (x' = -x + dx, z' = -z + dz)."""
    a = math.radians(ry)
    spots = []
    for ox, oz in zip(OFFX, OFFZ):
        vz = oz - ENTRY_BACK
        spots.append((-(p[0] + ox * math.cos(a) + vz * math.sin(a)) + dx,
                      -(p[2] - ox * math.sin(a) + vz * math.cos(a)) + dz))
    x, y, z = -p[0] + dx, p[1], -p[2] + dz
    cx, cz = sum(s[0] for s in spots) / 8, sum(s[1] for s in spots) / 8
    way_in = math.atan2(cx - x, cz - z)
    act = EXIT_ACT_BASE + GATES[did][2] - 15900
    return 'door %d %d %.2f %.2f %.2f %d %.4f %s %.4f' % (
        did, to, x, y, z, act, way_in + math.pi, ';'.join('%.2f,%.2f' % s for s in spots), way_in)


def final_index(k):
    """HMD 1004 triangle k -> its index after stage_wrap's dummies (no index may end in 0xFF)."""
    return k + k // 255


def build(unpacked, pac11, work, mod, rooms=WORLD_ROOMS, name='HCTP Backstage', mod_id=None, progress=lambda p, t: None):
    import world_stage
    out = os.path.join(work, 'out')
    mot = os.path.join(out, 'mot')
    os.makedirs(mot, exist_ok=True)
    os.makedirs(mod, exist_ok=True)
    bg78 = os.path.join(pac11, 'bg', 'bg78.pac')
    places = []
    for k, room in enumerate(rooms):
        dx, dz, crop, _ = B.ROOMS[room]
        places.append((dx, dz, crop))
        fake = os.path.join(out, 'wfake56_%d_u%d.pac' % (room, TEX_UPSCALE))
        if not os.path.exists(fake):
            progress(5 + 20 * k // len(rooms), 'room %d models' % room)
            B.py([os.path.join(HERE, 'hctp_fake56.py'), os.path.join(unpacked, 'bg', 'stg_00%d' % room), bg78, fake,
                  '--report', '--climb-df'], env={'HCTP_UNPACKED': unpacked, 'HCTP_TEX_UPSCALE': str(TEX_UPSCALE), 'HCTP_DROP_W': '1'},
                 out=os.path.join(out, 'wfake56_%d_u%d.log' % (room, TEX_UPSCALE)))
    progress(30, 'stage')
    st = world_stage.build(bg78, os.path.join(mod, 'arena.pac'),
                           [(os.path.join(out, 'wfake56_%d_u%d.pac' % (r, TEX_UPSCALE)), (p[0], p[1]), p[2]) for r, p in zip(rooms, places)])
    open(os.path.join(out, 'world_stage.log'), 'w').write('\n'.join(st['log']) + '\n')
    # the motion bank: room 30's (bank_h3) + every HCTP backstage act, then each room's object acts
    progress(45, 'motions')
    B.room_bank(31, unpacked, pac11, mot)            # (bank_wx.bin on the way)
    bank = os.path.join(mot, 'bank_wx.bin')
    conv = os.path.join(HERE, 'mot', 'hctp2ymks.py')
    for room in rooms:
        objs = os.path.join(unpacked, 'bg', 'stg_00%d' % room, '50102.bin')
        if room == 30 or not os.path.exists(objs):
            continue
        nb = os.path.join(mot, 'world_%d.bin' % room)
        B.py([conv, objs, B.acts(objs, OBJ_ACT[room], lambda n: True), '--objects', '--into', bank, '--out', nb])
        bank = nb
    # each room's props (one GMPD record range, one triangle range each), then one package
    spec, spots, tex, text, weapons, drops = [], [], {}, [], [], set()
    room_lines = []                    # (text index, room line without its climb-spot records, spot count)
    for k, room in enumerate(rooms):
        progress(55 + 30 * k // len(rooms), 'room %d props' % room)
        dx, dz, _ = places[k]
        rw = os.path.join(work, 'w%d' % room)
        rmod = os.path.join(rw, 'mod')
        os.makedirs(rmod, exist_ok=True)
        part = os.path.join(rw, 'spec.pkl')
        B.py([os.path.join(HERE, 'build_props.py')], cwd=rw,
             env={'HCTP_BANK': bank, 'HCTP_ROOM': str(room), 'HCTP_DX': str(dx), 'HCTP_DZ': str(dz), 'HCTP_OUT': rmod,
                  'HCTP_UNPACKED': unpacked, 'SVR11_PAC': pac11, 'HCTP_OBJ_ACT': str(OBJ_ACT.get(room, 12700)),
                  'HCTP_REC_BASE': str(100 + len(spec)), 'HCTP_TRI_BASE': str(st['starts'][k]),
                  'HCTP_SPEC_OUT': part, 'HCTP_TEX_PREFIX': 'p%d' % k, 'HCTP_TEX_UPSCALE': str(TEX_UPSCALE)},
             out=os.path.join(out, 'world_props_%d.log' % room))
        got = pickle.load(open(part, 'rb'))
        o0 = FIRST_OBJ + len(spec)
        spec += got['spec']
        spots += got['spots']
        tex.update(got['tex'])
        x0, x1, z0, z1 = st['areas'][k]
        lo, hi = 20 * k, 20 * k + 19
        t0, t1 = st['starts'][k], (st['starts'][k + 1] if k + 1 < len(rooms) else st['triangles'])
        room_lines.append((len(text), 'room %d %d %d %d %d %d %.1f %.1f %.1f %.1f %d %d' % (
            room, lo, hi, o0, FIRST_OBJ + len(spec) - 1, OBJ_ACT.get(room, 12700), x0, x1, z0, z1,
            final_index(t0), final_index(t1 - 1)), len(got['spots'])))
        text.append(None)
        gates = dict(GATES)
        if 35 in rooms and 34 not in rooms:
            gates.update(GATES_FOUNTAIN)
        for did, p, ry in doors_of(os.path.join(unpacked, 'bg', 'stg_00%d' % room)):
            if did not in gates or gates[did][0] not in rooms:
                continue                       # (leads to a room left out - or, 201, to the arena)
            text.append(gate_line(did, gates[did][1], p, ry, dx, dz))
        text += [l for l in open(os.path.join(rmod, 'hctp.txt')).read().splitlines() if l.strip()]
        w = open(os.path.join(rmod, 'weapons.txt')).read().strip()
        if w and w != 'none':
            if k == 0:
                weapons.append(w)
            else:
                text += ['spawn ' + ' '.join(v.split(',')[:4]) for v in w.split(';')]
        drops |= {int(l.split()[2]) for l in text if l and l.startswith('weapon ')}
    # climb-spot records follow the props' in the package, room by room
    first = 100 + len(spec)
    for at, line, n in room_lines:
        text[at] = '%s %d %d' % (line, first, first + n - 1)
        first += n
    progress(88, 'package')
    import hctp_gimmick
    # (its own GMGB entry: 78WB the backstage, 78WF the fountain one - not a room's, 78R0 ..)
    entry = '78WF' if 35 in rooms else '78WB'
    hctp_gimmick.build(os.path.join(pac11, 'gm.pac'), os.path.join(mod, 'gimmick.pac'), entry=entry, props=spec,
                       textures=tex, records=spots, bank=open(bank, 'rb').read(), keep_models=False,
                       first_obj=FIRST_OBJ, last_obj=LAST_OBJ)
    open(os.path.join(mod, 'hctp.txt'), 'w').write('\n'.join(text) + '\n')
    x0, x1, z0, z1 = st['areas'][0]
    mod_id = mod_id or re.sub(r'[^A-Za-z0-9_]', '_', os.path.basename(os.path.normpath(mod))) or 'hctp_world'
    lines = ['type=backstage', 'id=%s' % mod_id, 'name=%s' % name, 'author=HCTP port', 'version=1.0', 'area=0',
             'row=%s' % name, 'gimmick=%s' % entry,
             'box=%.1f,%.1f,%.1f,%.1f' % ((x0 + x1) / 2, (z0 + z1) / 2, (x1 - x0) / 2, (z1 - z0) / 2),
             'camera_height=45', 'hctp=%d' % rooms[0], 'weapons=%s' % (';'.join(weapons) or 'none')]
    # (HCTP's gates - exit acts, 8 spots - are in the game from NEEDS_GATES; a stage bigger than the shipped
    # bg78 needs the big-model memory, model_budget.py; one over the window, NEEDS_BIG_STAGE)
    import model_budget
    needs = [NEEDS_GATES]
    if st['file'] > st['orig_file']:
        needs.append(model_budget.NEEDS_VERSION)
    if len(rooms) > 5 or any(r in B.BIG_STAGE_ROOMS and not B.ROOMS[r][2] for r in rooms):
        needs.append(B.NEEDS_BIG_STAGE)
    lines.insert(5, 'needs=%s' % max(needs, key=lambda v: tuple(int(n) for n in v.split('.'))))
    if drops:
        lines.append('load_weapons=%s' % ','.join(map(str, sorted(drops))))
    open(os.path.join(mod, 'manifest.txt'), 'w', newline='').write('\r\n'.join(lines) + '\r\n')
    progress(100, 'done')
    return {'rooms': list(rooms), 'id': mod_id, 'objects': len(spec), 'files': {n: os.path.getsize(os.path.join(mod, n))
                                                                  for n in os.listdir(mod)}}


if __name__ == '__main__':
    a = sys.argv[1:]
    import json
    rooms = tuple(int(r) for r in a[4].split(',')) if len(a) > 4 else WORLD_ROOMS
    print(json.dumps(build(a[0], a[1], a[2], a[3], rooms, progress=lambda p, t: B.log('progress', p, t)), indent=1))
