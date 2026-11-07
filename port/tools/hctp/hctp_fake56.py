"""HCTP backstage room -> a stand-in "bg56" EPAC that svr08_stage.py builds into bg78.

    python hctp_fake56.py <HCTP stage dir> <2011 bg78.pac> <out fake.pac> [--tone t] [--report]

The fake holds STG/0056 PACH with: room models as JBOY at parking ids (0-18,
skipping 6/7/11/16 which the game hides at times), 200 = DDS texture bundle
(with mips), 996 = HCTP 00999 collision, 50001 = flags (HCTP letters; k -> s).
Blend-alpha submeshes of the opaque room models go to one extra model (flag s).
"""
import io
import struct
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # (its own folder: the embeddable Python doesn't add it)
import svrfmt as f  # noqa: E402
import jboy  # noqa: E402
import arena_tool as at  # noqa: E402
import svr08_stage as s8  # noqa: E402

import hctp2jboy  # noqa: E402
import yobj  # noqa: E402

# HCTP room model -> (fake id, flags)
ROOM_30 = {
    1: (0, 'n p'), 2: (1, 'n w'), 3: (2, 'n w'), 4: (3, 'n'), 11: (4, 'n'), 12: (5, 'n'),
    13: (8, 'n'), 16: (9, 'n'), 18: (10, 'n'), 30: (12, 'n s'), 31: (13, 'n s'),
    40: (14, 'n a z'), 42: (17, 'n'),   # (41 hansha: HCTP's floor reflection - a white streak in 2011)
}
ALPHA_ID = 18
# parking model ids free for room models (the game hides 6, 7, 11 and 16 at times; 18 = the alpha parts)
FREE_IDS = (0, 1, 2, 3, 4, 5, 8, 9, 10, 12, 13, 14, 15, 17, 19)
MAX_MESHES = 64


def room_table(stage):
    """Any room: {fake id: ([HCTP room model ids], flags)} from its 50001 flag list - event-only
    models ('e') left out, models with the same flags merged into one (the parking slot has 15
    free ids), unlisted room models (< 100) as 'n'."""
    import os
    import re
    fl = {}
    name = '50001.bin' if os.path.exists(os.path.join(stage, '50001.bin')) else '50001.txt'
    raw = open(os.path.join(stage, name), 'rb').read().decode('shift_jis', 'replace')
    for line in raw.splitlines():
        line = line.split(';')[0].split('//')[0].strip()
        m = re.match(r'(\d+)\s*(.*)', line)
        if m:
            fl[int(m.group(1))] = ' '.join(m.group(2).split()) or 'n'
    ids = sorted(int(n[:5]) for n in os.listdir(stage) if n.endswith('.yobj') and int(n[:5]) < 100)
    groups = {}
    for i in ids:
        f_ = fl.get(i, 'n')
        if 'e' in f_.split() or str(i) in os.environ.get('HCTP_SKIP_MODELS', '').split(','):  # (test aid: skip ids)
            continue
        # ('w': shown only by HCTP's move cameras, never in play - HCTP_DROP_W leaves them out: the world
        # stage's mesh count)
        if 'w' in f_.split() and os.environ.get('HCTP_DROP_W'):
            continue
        # (an additive layer driven by a light motion - room 35's flood water, which never floods
        # in HCTP: 2011 draws it as an opaque sheet over the square)
        if 'a' in f_.split() and 'l(' in f_:
            continue
        groups.setdefault(f_, []).append(i)
    if len(groups) > len(FREE_IDS):
        raise SystemExit(f'{len(groups)} model groups, {len(FREE_IDS)} free ids')
    return {fid: (hids, f_) for fid, (f_, hids) in zip(FREE_IDS, sorted(groups.items(), key=lambda g: g[1][0]))}


def merge_jboy(parts, name):
    """JBOY models -> one (meshes appended, texture slots remapped onto one list)."""
    merged = None
    for J in parts:
        if merged is None:
            merged = J
            continue
        for s in J.meshes:
            k = [i for i, (n, t, v) in enumerate(s.params) if n == 'texDiffuse'][0]
            slot = struct.unpack('>i', s.params[k][2][:4])[0]
            tn = J.textures[slot]
            if tn not in merged.textures:
                merged.textures.append(tn)
            n, t, v = s.params[k]
            s.params[k] = (n, t, struct.pack('>i', merged.textures.index(tn)) + v[4:])
            merged.meshes.append(s)
    merged.name = name
    merged.nodes[0]['name'] = name
    merged.nodes[0]['sphere'] = hctp2jboy.sphere_of([v[:3] for s in merged.meshes for v in s.verts])
    return merged


def two_level_cut(img):
    """A texture whose alpha has only a few values, mostly two levels with the
    top one opaque (kanami2 152/255, sunoko2 54/255): drawn with the PS2 alpha
    test, which drops the lower level -> the cut between the levels, else None."""
    u, c = np.unique(img[..., 3], return_counts=True)
    if len(u) > 6 or u[-1] != 255:
        return None
    lv = [int(v) for v, n in zip(u, c) if n >= 0.01 * c.sum()]
    if len(lv) != 2 or lv[0] == 0:
        return None
    return (lv[0] + 255) // 2


def alpha_kind(img):
    a = img[..., 3]
    if two_level_cut(img):
        return 'cut'
    # (kanami2, the cage poles' strip: alpha 152 over most of it - a soft dark edge, not opaque)
    if (a < 128).mean() < 0.002 and (a < 250).mean() < 0.1:
        return 'opaque'
    mid = ((a > 16) & (a < 240)).mean()
    return 'cut' if mid < 0.03 else 'blend'


def bleed(img, steps=8):
    """Transparent texels take their visible neighbours' colour (wrapped), so
    filtering doesn't pull the black they store into cut-out edges."""
    rgb = img[..., :3].astype(np.float32)
    known = img[..., 3] >= 128
    if known.all() or not known.any():
        return img
    for _ in range(steps):
        acc = np.zeros_like(rgb)
        cnt = np.zeros(known.shape, np.float32)
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0), (1, 1), (1, -1), (-1, 1), (-1, -1)):
            k = np.roll(known, (dy, dx), (0, 1))
            acc += np.roll(rgb, (dy, dx), (0, 1)) * k[..., None]
            cnt += k
        grow = ~known & (cnt > 0)
        rgb[grow] = acc[grow] / cnt[grow][:, None]
        known = known | grow
        if known.all():
            break
    out = img.copy()
    out[..., :3] = np.clip(rgb + 0.5, 0, 255).astype(np.uint8)
    return out


def upscale(img, k):
    """k x larger, smooth (Lanczos) with the texture's tiling wrapped round, so
    the PS2 texels and the DXT 4x4 blocks don't show as squares when magnified."""
    h, w = img.shape[:2]
    if k <= 1:
        return Image.fromarray(img, 'RGBA')
    big = Image.fromarray(np.tile(img, (3, 3, 1)), 'RGBA').resize((3 * w * k, 3 * h * k), Image.LANCZOS)
    return big.crop((w * k, h * k, 2 * w * k, 2 * h * k))


# (HCTP_TEX_UPSCALE: at most this upscale, see-through textures half of it - the world stage uses 2: its
# textures, its props' and their GPU buffers share one memory window)
UPSCALE = int(os.environ.get('HCTP_TEX_UPSCALE', '4'))


def dds(img, alpha):
    h0, w0 = img.shape[:2]
    k = 1
    while k < (min(UPSCALE, 4) if alpha == 'opaque' else max(1, UPSCALE // 2)) and max(w0, h0) * k * 2 <= (512 if alpha == 'opaque' else 256):
        k *= 2
    src = img
    if alpha == 'cut' and two_level_cut(img):  # (the lower level is a hole: no colour of its own)
        src = img.copy()
        src[..., 3] = np.where(img[..., 3] >= two_level_cut(img), 255, 0)
    im = upscale(bleed(src) if alpha != 'opaque' else src, k)
    if alpha == 'cut':             # keep cut-outs crisp: alpha back to 0 / 255
        thr = two_level_cut(img) or 128
        r, g, b, a = im.split()
        im = Image.merge('RGBA', (r, g, b, a.point(lambda v: 255 if v >= thr else 0)))
    w, h = im.size
    fmt = 'DXT5' if alpha == 'blend' else ('DXT1' if alpha == 'opaque' else 'DXT5')
    levels = []
    while True:
        levels.append(im)
        # see-through textures (chain-link, fences): one level only - smaller mips
        # average the holes' alpha below the cut-off and the fence vanishes at a distance
        if alpha != 'opaque' or (w <= 4 and h <= 4):
            break
        w, h = max(1, w // 2), max(1, h // 2)
        im = im.resize((w, h), Image.BOX)
    out = None
    for lv in levels:
        b = io.BytesIO()
        lv.save(b, 'DDS', pixel_format=fmt)
        v = b.getvalue()
        out = bytearray(v) if out is None else out + v[128:]
    struct.pack_into('<I', out, 8, struct.unpack_from('<I', out, 8)[0] | 0x20000)
    struct.pack_into('<I', out, 28, len(levels))
    struct.pack_into('<I', out, 108, struct.unpack_from('<I', out, 108)[0] | 0x400008)
    return bytes(out)


class TexBank(dict):
    """A room's texture bank; a name the bank lacks (room 32's doa_nob2: HCTP leaves it unset)
    gets its nearest sibling (same name without the trailing digits), else a grey texel."""

    def __missing__(self, name):
        base = name.rstrip('0123456789')
        for k in sorted(self):
            if k.startswith(base):
                print(f'texture {name}: not in the bank, using {k}')
                self[name] = self[k]
                return self[k]
        print(f'texture {name}: not in the bank, grey')
        self[name] = np.full((4, 4, 4), 128, np.uint8)
        return self[name]


def copy_of(M, kind):
    """A room model's second copy: 'r' mirrored in the YZ plane (x -> -x, each strip's winding
    turned by a leading repeat of its first vertex), 'y' turned 180 about Y."""
    import copy
    C = copy.copy(M)
    C.meshes = []
    flip = np.array([-1.0, 1.0, 1.0]) if kind == 'r' else np.array([-1.0, 1.0, -1.0])
    for m in M.meshes:
        n = dict(m, pos=m['pos'][:, :3] * flip if m['pos'].shape[1] == 3 else
                 np.concatenate([m['pos'][:, :3] * flip, m['pos'][:, 3:]], 1),
                 nrm=m['nrm'][:, :3] * flip if m['nrm'].shape[1] == 3 else
                 np.concatenate([m['nrm'][:, :3] * flip, m['nrm'][:, 3:]], 1))
        if kind == 'r':
            n['subs'] = [dict(s, strips=[[st[0]] + list(st) for st in s['strips'] if len(st)]) for s in m['subs']]
        C.meshes.append(n)
    return C


def texbank(stage):
    return TexBank((n, img) for n, img in yobj.texbank(open(stage + '/00200.bin', 'rb').read()).items()
                   if img is not None)


def split_alpha(M, kinds):
    """-> (opaque model, alpha-only model) as yobj models sharing data."""
    import copy
    op, al = copy.copy(M), copy.copy(M)
    op.meshes, al.meshes = [], []
    for m in M.meshes:
        so = [s for s in m['subs'] if kinds.get(M.textures[s['tex']]) != 'blend']
        sa = [s for s in m['subs'] if kinds.get(M.textures[s['tex']]) == 'blend']
        if so:
            op.meshes.append(dict(m, subs=so))
        if sa:
            al.meshes.append(dict(m, subs=sa))
    return op, al


def build(stage, p78, out, tone=1.0, report=False, with_props=True, climb_remap=None):
    _, _, _, _, e78 = s8.stage(p78)
    d78 = {i: s8.unpack(b) for i, b in e78}
    tmpl = hctp2jboy.template(d78)
    tb = texbank(stage)
    kinds = {n: alpha_kind(img) for n, img in tb.items() if img is not None}

    def see_through(t):   # (cut-outs - fences, grilles, foliage: both sides; solid walls stay culled, so a
        return kinds.get(t) in ('cut', 'blend')   # camera outside a room still sees in)
    models = {}
    flags = {}
    alpha_parts = []
    import os
    table = ({fid: ([hid], fl) for hid, (fid, fl) in ROOM_30.items()} if os.path.basename(os.path.normpath(stage)).endswith('0030')
             else room_table(stage))
    for fid, (hids, fl) in table.items():
        parts = []
        for hid in hids:
            M = yobj.read(open(f'{stage}/{hid:05d}.yobj', 'rb').read())
            # HCTP draws an 'r' model a second time mirrored in the YZ plane and a 'y' one turned
            # 180 about Y (rooms 35 / 36: one half of the fountain square); 2011 has no such
            # flags, so the copies are baked in
            for Mi in [M] + [copy_of(M, k) for k in ('r', 'y') if k in fl.split()]:
                if 's' in fl.split() or 'a' in fl.split():
                    op, al = Mi, None
                else:
                    op, al = split_alpha(Mi, kinds)
                # (additive layers - 'a', drawn plain translucent - at a quarter: street-light pools and
                # Times Square's traffic-light glows covered the street in solid colour)
                J = (hctp2jboy.convert(op, tmpl, tone=tone, double_sided=see_through,
                                       alpha=0.25 if 'a' in fl.split() else 1.0, both_below=30.0) if op.meshes else None)
                if J:
                    parts.append(J)
                if al is not None and al.meshes:
                    alpha_parts.append(al)
        if parts:
            models[fid] = merge_jboy(parts, parts[0].name) if len(parts) > 1 else parts[0]
            # (only the letters 2011 knows: r / y are baked, l(..) / m(..) name HCTP motions. An additive
            # layer ('a': street-light cones and pools) is drawn by 2011 in its light motion's colour - flat
            # green over Times Square's street, without one or with stock l(101) alike: plain translucent
            # ('s') instead, a soft haze)
            fl_ = [('s' if k == 'a' else k) for k in fl.split() if k in os.environ.get('HCTP_FLAGS_KEEP', 'nsakzwp')]
            flags[fid] = ' '.join(dict.fromkeys(fl_)) or 'n'
    # a model of more than MAX_MESHES meshes (room 34's ground: 99) is split over free ids
    # (room 34 crashed at load with it whole; 71 load fine)
    for fid in sorted(models):
        while len(models[fid].meshes) > MAX_MESHES:
            free = [i for i in FREE_IDS if i not in models]
            if not free:
                raise SystemExit('no free id to split a big model')
            import copy
            J = models[fid]
            rest = copy.copy(J)
            rest.meshes = J.meshes[MAX_MESHES:]
            J.meshes = J.meshes[:MAX_MESHES]
            for part in (J, rest):
                part.nodes = [dict(n) for n in part.nodes]
                part.nodes[0]['sphere'] = hctp2jboy.sphere_of([v[:3] for s in part.meshes for v in s.verts])
            models[free[0]], flags[free[0]] = rest, flags[fid]
    if alpha_parts:
        import copy
        A = copy.copy(alpha_parts[0])
        A.meshes = [m for p in alpha_parts for m in p.meshes]
        # textures are per source model: convert each and merge meshes
        merged = None
        for p in alpha_parts:
            J = hctp2jboy.convert(p, tmpl, tone=tone, double_sided=True)
            if merged is None:
                merged = J
                merged.name = 'hctp_alpha'
                continue
            for s in J.meshes:
                k = [i for i, (n, t, v) in enumerate(s.params) if n == 'texDiffuse'][0]
                slot = struct.unpack('>i', s.params[k][2][:4])[0]
                tn = J.textures[slot]
                if tn not in merged.textures:
                    merged.textures.append(tn)
                n, t, v = s.params[k]
                s.params[k] = (n, t, struct.pack('>i', merged.textures.index(tn)) + v[4:])
                merged.meshes.append(s)
        merged.nodes[0]['name'] = 'hctp_alpha'
        merged.nodes[0]['sphere'] = hctp2jboy.sphere_of([v[:3] for s in merged.meshes for v in s.verts])
        models[ALPHA_ID] = merged
        flags[ALPHA_ID] = 'n s'
    used = sorted({t for m in models.values() for t in m.textures})
    bundle = at.tex_bundle_write([(t + '.dds', dds(tb[t], kinds.get(t) or alpha_kind(tb[t]))) for t in used])
    hmd = open(stage + '/00999.hmd', 'rb').read()
    if with_props:
        import hctp_props
        extra = hctp_props.world_collision(stage, hctp_props.props(stage, int(os.path.basename(os.path.normpath(stage))[-2:])))
        hmd = s8.hmd_write(s8.hmd_read(hmd) + extra) if extra else hmd
        if report:
            print(f'collision: 00999 + {len(extra)} prop triangles at rest')
    if climb_remap:
        hb = bytearray(hmd)
        n = struct.unpack_from('<I', hb, 4)[0]
        changed = 0
        for k in range(n):
            o = 8 + 96 * k + 92
            w = struct.unpack_from('<I', hb, o)[0]
            if (w & 0xFF) in climb_remap:
                struct.pack_into('<I', hb, o, (w & ~0xFF) | climb_remap[w & 0xFF])
                changed += 1
        hmd = bytes(hb)
        if report:
            print(f'collision: {changed} climb faces remapped {climb_remap}')
    flag_text = ('\r\n'.join(f'{i} {flags[i]}' for i in sorted(flags)) + '\r\n').encode()
    flag_text += b'//' + b'-' * 120 + b'\r\n'  # (a short list doesn't BPE-compress)
    entries = [(i, f.bpe_pack(jboy.write(m))) for i, m in sorted(models.items())]
    entries += [(200, f.bpe_pack(bundle)), (996, f.bpe_pack(hmd)), (50001, f.bpe_pack(flag_text))]
    h78 = open(p78, 'rb').read()[:0x800]
    data = f.epac_write(h78, [(b'STG ', [(b'0056', f.pach_write(entries))])])
    open(out, 'wb').write(data)
    if report:
        for i, m in sorted(models.items()):
            print(f'{i:3d} {m.name:14s} meshes {len(m.meshes):3d} verts {sum(len(s.verts) for s in m.meshes):6d} '
                  f'textures {len(m.textures):3d}  flags {flags[i]}')
        print(f'textures {len(used)}, bundle {len(bundle)} bytes; fake {len(data)} bytes')
    return models


if __name__ == '__main__':
    a = sys.argv[1:]
    tone = float(a[a.index('--tone') + 1]) if '--tone' in a else 1.0
    pos = [x for x in a if not x.startswith('--') and x != (str(tone) if '--tone' in a else None)]
    cr = {0xFB: 0xFE, 0xFD: 0xFE, 0xF4: 0xFE, 0xDF: 0xFE} if '--climb-fe' in a else None
    if '--climb-df' in a:
        cr = {0xFB: 0xDF, 0xFD: 0xDF, 0xF4: 0xDF}
    build(pos[0], pos[1], pos[2], tone=tone, report='--report' in a, climb_remap=cr)
