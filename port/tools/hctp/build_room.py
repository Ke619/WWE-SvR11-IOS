"""An HCTP backstage room -> a SvR2011 backstage row mod (Mods/Backstage/<name>), all in Python
(build_room.sh / make_bank.sh did it in sh; tools/backstage_port.py calls build()).

    py build_room.py <room 30..36> <HCTP unpacked folder> <2011 pac folder> <work folder> <mod folder>

Inputs: the HCTP files hctp_unpack.py writes (bg/stg_00NN, gm/GMMC_GOBJ, m/MOT_GAME) and 2011's
pac/bg/bg78.pac + pac/gm.pac (the player's own game). The room goes into bg78's parking slot turned a
half turn, its centre on the parking fight-box centre (125, -410) (ROOMS: offset, optional crop. Room 34 was
cropped for 2011's stage vertex window until a stage over it was placed elsewhere, 2.1.5 - src/model_mem.cpp).
"""
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # (its own folder: the embeddable Python doesn't add it)
sys.path.insert(0, os.path.join(HERE, 'mot'))

# the first game version that loads a stage over its vertex window (src/model_mem.cpp): what such a room says
# in its manifest (needs=) - older games crash at its load
NEEDS_BIG_STAGE = '2.1.5'


def gimmick_entry(room):
    """the room's own GMGB entry (78R0 .. 78R6): every installed HCTP mod gets its gimmicks, arena_mods.cpp
    (before 2.1.6 every room was 78H3 and only the first mod's played)"""
    return '78R%d' % (room - 30)
BIG_STAGE_ROOMS = {34}   # (uncropped: over the window)
# room -> (dx, dz, crop x0,x1,z0,z1 or None, row name)
ROOMS = {
    30: (180.0, -440.0, None, 'Back Entrance'),
    31: (140.0, -437.7, None, 'Parking Lot'),
    32: (103.5, -388.0, None, 'Training Room'),
    33: (86.5, -408.0, None, 'Times Square'),
    34: (205.0, -334.0, None, 'Outside the Arena'),
    35: (235.0, -617.5, None, 'Fountain Square'),
    36: (235.0, -607.0, None, 'Fountain Square (dry)'),
}
# jumps to motions 2011 has under the same number (the downs 443 / 448 / 476; 1660, 1662, 1900, 683 it lacks)
JUMPMAP = '0=0,1=1,153=153,443=443,448=448,476=476'


def log(*a):
    print(*a, file=sys.stderr, flush=True)


def py(args, env=None, cwd=None, out=None):
    """A tool script in this Python; its output to the file out (or the log)."""
    r = subprocess.run([sys.executable] + [str(a) for a in args], env={**os.environ, **(env or {})}, cwd=cwd,
                       capture_output=True, text=True)
    if out:
        open(out, 'w').write(r.stdout + r.stderr)
    if r.returncode:
        raise RuntimeError('%s failed: %s' % (os.path.basename(str(args[0])), (r.stderr or r.stdout)[-600:]))
    return r.stdout


def acts(bank_path, add, keep):
    """'id=id+add,...' for the motions of an HCTP bank whose id passes keep"""
    import hbank
    _, e, _ = hbank.streams(open(bank_path, 'rb').read())
    return ','.join('%d=%d' % (k, k + add) for k in sorted({x['id'] for x in e}) if keep(k))


def bank_78pk(pac11, out):
    import svrfmt as f
    import svr08_gimmick as G
    _, _, pk = G.find(os.path.join(pac11, 'gm.pac'), b'GMGB', b'78PK')
    top = dict(f.pach_read(pk))
    sub = {i: G.unpack(b) for i, b in f.pach_read(top[0])}
    open(out, 'wb').write(sub[5])


def bank_h3(unpacked, pac11, mot):
    """Room 30's bank (make_bank.sh): 2011's 78PK + HCTP's climbs / cage / gimmick acts + room 30's objects."""
    out = os.path.join(mot, 'bank_h3.bin')
    if os.path.exists(out):
        return out
    hb = os.path.join(unpacked, 'm', 'MOT_GAME')
    conv = os.path.join(HERE, 'mot', 'hctp2ymks.py')
    bank_78pk(pac11, os.path.join(mot, '78PK_bank.bin'))
    py([conv, os.path.join(hb, '00000.bin'), '256=12501,262=12502,263=12503,664=12504,340=12516',
        '--into', os.path.join(mot, '78PK_bank.bin'), '--out', os.path.join(mot, 'b1.bin'), '--jumpmap', '0=0,1=1,153=153'])
    py([conv, os.path.join(hb, '00004.bin'),
        '12572=12505,12092=12506,12045=12507,12046=12508,12047=12509,12035=12510,12036=12511,12037=12512,'
        '12034=12513,12043=12514,12014=12515,12060=12545',
        '--into', os.path.join(mot, 'b1.bin'), '--out', os.path.join(mot, 'b2.bin'), '--jumpmap', '0=0,1=1,153=153'])
    py([conv, os.path.join(unpacked, 'bg', 'stg_0030', '50102.bin'),
        '23=12523,24=12524,25=12525,26=12526,27=12527,28=12528,29=12529,30=12530,31=12531,32=12532,33=12533',
        '--objects', '--into', os.path.join(mot, 'b2.bin'), '--out', os.path.join(mot, 'b3.bin')])
    sys.path.insert(0, HERE)
    import bank_rekey
    d = open(os.path.join(mot, 'b3.bin'), 'rb').read()
    m = {(12501, 1): (12541, 0), (12501, 2): (12542, 0), (12502, 2): (12544, 0), (12502, 1): (12543, 0),
         (12528, 0): (12558, 0), (12526, 0): (12556, 0), (12525, 0): (12555, 0), (12524, 0): (12554, 0)}
    fr = {(12558, 0): 1, (12556, 0): 1, (12555, 0): 1, (12554, 0): 1}
    d = bank_rekey.rekey(d, m, fr)
    d = bank_rekey.copy_track(d, (12513, 0, 2), (12559, 0, 0), frames=1)  # (the equipment crate's rest pose)
    open(out, 'wb').write(d)
    return out


def room_bank(room, unpacked, pac11, mot):
    """Room 30: bank_h3. Others: bank_h3 + every HCTP backstage act 12000-12099 as 12600 + n (12500-12599,
    the high-ground acts, as 12800 + n; 15900-15919, the gates' exit acts, as 12900 + n - build_world.py) + the
    room's object acts (50102) n as 12700 + n."""
    h3 = bank_h3(unpacked, pac11, mot)
    if room == 30:
        return h3
    conv = os.path.join(HERE, 'mot', 'hctp2ymks.py')
    hb = os.path.join(unpacked, 'm', 'MOT_GAME', '00004.bin')
    wr = os.path.join(mot, 'bank_wx.bin')
    if not os.path.exists(wr):
        ids = ','.join((acts(hb, 600, lambda k: 12000 <= k < 12100), acts(hb, 300, lambda k: 12500 <= k < 12600),
                        acts(hb, -3000, lambda k: 15900 <= k < 15920)))
        py([conv, hb, ids, '--into', h3, '--out', wr, '--jumpmap', JUMPMAP])
    out = os.path.join(mot, 'bank_r%d.bin' % room)
    objs = os.path.join(unpacked, 'bg', 'stg_00%d' % room, '50102.bin')
    if os.path.exists(objs):
        py([conv, objs, acts(objs, 12700, lambda k: True), '--objects', '--into', wr, '--out', out])
    else:
        shutil.copy(wr, out)
    return out


def build(room, unpacked, pac11, work, mod, name=None, progress=lambda p, t: None, mod_id=None):
    """-> report dict; mod gets arena.pac, gimmick.pac, hctp.txt, manifest.txt."""
    import svrfmt as f
    import svr08_stage as s8
    dx, dz, crop, row = ROOMS[room]
    name = name or '%s (HCTP)' % row
    stg = os.path.join(unpacked, 'bg', 'stg_00%d' % room)
    bg78 = os.path.join(pac11, 'bg', 'bg78.pac')
    mot = os.path.join(work, 'out', 'mot')
    os.makedirs(mot, exist_ok=True)
    os.makedirs(mod, exist_ok=True)
    out = os.path.join(work, 'out')
    progress(10, 'room models')
    fake = os.path.join(out, 'fake56_%d.pac' % room)
    py([os.path.join(HERE, 'hctp_fake56.py'), stg, bg78, fake, '--report', '--climb-df'],
       env={'HCTP_UNPACKED': unpacked}, out=os.path.join(out, 'fake56_%d.log' % room))
    progress(35, 'stage')
    wrapped = os.path.join(out, 'bg78_hctp%d.pac' % room)
    stage_log = os.path.join(out, 'bg78_hctp%d.log' % room)
    args = [os.path.join(HERE, 'stage_wrap.py'), fake, bg78, wrapped, '--rotate', '180', '--offset', '%g,%g' % (dx, dz),
            '--drop-2011-cars', '--report']
    if crop:
        args[-1:-1] = ['--footprint', ','.join('%g' % v for v in crop)]
    py(args, out=stage_log)
    # the real stage first in STG; empty parking ids get stub models (the game looks some of them up by id)
    h, g, t = f.epac_read(open(wrapped, 'rb').read())
    ty, e = g[0]
    _, _, _, _, e78 = s8.stage(bg78)
    d78 = dict(e78)
    k = [i for i, (n, b) in enumerate(e) if n == b'0078'][0]
    ents = f.pach_read(e[k][1])
    have = {i for i, b in ents}
    ents += [(i, f.bpe_pack(s8.stub_model(s8.unpack(d78[i])))) for i in range(20) if i not in have and i in d78]
    ents.sort(key=lambda x: x[0])
    e[k] = (b'0078', f.pach_write(ents))
    open(os.path.join(mod, 'arena.pac'), 'wb').write(f.epac_write(h, [(ty, e)], t))
    progress(55, 'motions')
    bank = room_bank(room, unpacked, pac11, mot)
    progress(75, 'props')
    py([os.path.join(HERE, 'build_props.py')], cwd=work,
       env={'HCTP_BANK': bank, 'HCTP_ROOM': str(room), 'HCTP_DX': str(dx), 'HCTP_DZ': str(dz), 'HCTP_OUT': mod,
            'HCTP_UNPACKED': unpacked, 'SVR11_PAC': pac11},
       out=os.path.join(out, 'props_%d.log' % room))
    # the manifest: own row, the fight box = the room's extent, its weapons and the models its props drop
    m = re.search(r'x (-?[\d.]+)\.\.(-?[\d.]+), z (-?[\d.]+)\.\.(-?[\d.]+)\)', open(stage_log).read())
    x0, x1, z0, z1 = map(float, m.groups())
    wfile = os.path.join(mod, 'weapons.txt')
    weapons = open(wfile).read().strip() if os.path.exists(wfile) else 'none'
    if os.path.exists(wfile):
        os.remove(wfile)
    drops = sorted({int(l.split()[2]) for l in open(os.path.join(mod, 'hctp.txt')) if l.startswith('weapon ')})
    # (id: the install folder's name - the launcher installs a mod under Mods/<type>/<id>)
    mod_id = mod_id or re.sub(r'[^A-Za-z0-9_]', '_', os.path.basename(os.path.normpath(mod))) or 'hctp_room_%d' % room
    lines = ['type=backstage', 'id=%s' % mod_id, 'name=%s' % name, 'author=HCTP port', 'version=1.0', 'area=0',
             'row=%s' % name, 'gimmick=%s' % gimmick_entry(room),
             'box=%.1f,%.1f,%.1f,%.1f' % ((x0 + x1) / 2, (z0 + z1) / 2, (x1 - x0) / 2, (z1 - z0) / 2),
             'camera_height=45', 'hctp=%d' % room, 'weapons=%s' % weapons]
    if room in BIG_STAGE_ROOMS and not ROOMS[room][2]:
        lines.insert(5, 'needs=%s' % NEEDS_BIG_STAGE)
    if drops:
        lines.append('load_weapons=%s' % ','.join(map(str, drops)))
    open(os.path.join(mod, 'manifest.txt'), 'w', newline='').write('\r\n'.join(lines) + '\r\n')
    progress(100, 'done')
    sizes = {n: os.path.getsize(os.path.join(mod, n)) for n in os.listdir(mod)}
    return {'room': room, 'name': name, 'id': mod_id, 'files': sizes, 'weapons': weapons, 'stage_log': open(stage_log).read()[-400:]}


if __name__ == '__main__':
    a = sys.argv[1:]
    import json
    print(json.dumps(build(int(a[0]), a[1], a[2], a[3], a[4], progress=lambda p, t: log('progress', p, t)), indent=1))
