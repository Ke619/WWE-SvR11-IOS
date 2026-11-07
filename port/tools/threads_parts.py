"""Superstar Threads parts for a superstar mod: writes <mod>/threads.txt
(docs/SUPERSTAR_MODS.md, "Superstar Threads").

Threads paints a part where its mask channel is set: part p (1-12) of an attire
is channel (p-1) % 4 of the attire's Mask1/Mask2/Mask3 texture (in the model's
EMD entry, next to `color`). Models converted from WWE '12 / '13 / SvR 2010 bring
their masks; this names each used channel after the body area it covers
(bone weights of the vertices under it, and their height) with the game's own
part names (TRUNKS 1, KNEE PADS, BOOTS DESIGN ...), so the text is translated.
The default colour is the texture's colour under the mask.

A model without masks (SvR 2006-2009, PS2 ports) gets them with --make-masks:
painted from its clothes' colours (see "masks for models without them").

    threads_parts.py <mod folder> [--game <Game Files>] [--json] [--dry-run]
    threads_parts.py <mod folder> --make-masks [--dry-run --preview <png>]
    threads_parts.py --names [--game <Game Files>]     the usable part names (JSON)

--json prints {"ok", "file", "attires": [{"attire", "parts": [{"part", "name",
"id", "colour", "area"}]}], "masks": [{"pac", "attire", "channels", "info"}],
"message"} for the Mod Maker. A model without masks and no --make-masks gets
no file (exit 3).
"""
import argparse
import collections
import io
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ch_tool  # noqa: E402
import jboy  # noqa: E402
import svrfmt  # noqa: E402

GAME = os.path.join(os.path.dirname(os.path.dirname(HERE)), 'Game Files')
FORMAT = 1
AREAS = ('head', 'chest', 'upperarm', 'forearm', 'hand', 'hips', 'thigh', 'shin', 'foot')

# family -> (one base, numbered bases, one design, numbered designs)
FAMILIES = {
    'MASK': ('MASK', ['MASK 1', 'MASK 2'], 'MASK DESIGN', ['MASK DESIGN 1', 'MASK DESIGN 2', 'MASK DESIGN 3', 'MASK DESIGN 4']),
    'SHIRT': ('SHIRT', ['SHIRT 1', 'SHIRT 2', 'SHIRT 3'], 'SHIRT DESIGN', ['SHIRT DESIGN 1', 'SHIRT DESIGN 2', 'SHIRT DESIGN 3']),
    'SINGLET': ('SINGLET', ['SINGLET 1', 'SINGLET 2'], None,
                ['SINGLET DESIGN 1', 'SINGLET DESIGN 2', 'SINGLET DESIGN 3', 'SINGLET DESIGN 4']),
    'ELBOW PADS': ('ELBOW PADS', ['ELBOW PADS 1', 'ELBOW PADS 2'], 'ELBOW PADS DESIGN', []),
    'ARMBANDS': ('ARMBANDS', [], 'ARMBAND DESIGN', []),
    'WRIST TAPE': ('WRIST TAPE', ['WRIST TAPE 1', 'WRIST TAPE 2'], 'WRISTBANDS DESIGN', []),
    'GLOVES': ('GLOVES', ['GLOVES 1', 'GLOVES 2', 'GLOVES 3'], None, []),
    'TRUNKS': ('TRUNKS', ['TRUNKS 1', 'TRUNKS 2'], 'TRUNKS DESIGN',
               ['TRUNKS DESIGN 1', 'TRUNKS DESIGN 2', 'TRUNKS DESIGN 3', 'TRUNKS DESIGN 4']),
    'TIGHTS': ('TIGHTS', ['TIGHTS 1', 'TIGHTS 2', 'TIGHTS 3'], 'TIGHTS DESIGN',
               ['TIGHTS DESIGN 1', 'TIGHTS DESIGN 2', 'TIGHTS DESIGN 3', 'TIGHTS DESIGN 4']),
    'KNEE PADS': ('KNEE PADS', ['KNEE PADS 1', 'KNEE PADS 2', 'KNEE PADS 3'], None,
                  ['KNEE PADS DESIGN 1', 'KNEE PADS DESIGN 2']),
    'KICKPADS': ('KICKPADS', [], 'KICKPADS DESIGN',
                 ['KICKPADS DESIGN 1', 'KICKPADS DESIGN 2', 'KICKPADS DESIGN 3', 'KICKPADS DESIGN 4']),
    'BOOTS': ('BOOTS', ['BOOTS 1', 'BOOTS 2', 'BOOTS 3'], 'BOOTS DESIGN', ['BOOTS DESIGN 1', 'BOOTS DESIGN 2']),
    'SOLES': ('SOLES', [], None, []),
}


_unpack = svrfmt.unbpe


# ---- the game's part names: SSTL ids (edit.pac) with their text (string.pac, English group 0600)

def game_names(game):
    """-> {name: id} for every part name a stock Threads record uses"""
    _, groups, _ = svrfmt.epac_read(open(os.path.join(game, 'pac', 'edit', 'edit.pac'), 'rb').read())
    sstl = next(b for t, ents in groups if t == b'EDIT' for n, b in ents if n == b'SSTL')
    rec = _unpack(svrfmt.pach_read(_unpack(sstl))[0][1])
    count = rec[0] | rec[1] << 8
    ids = set()
    for i in range(count):
        ids.update(x for x in struct.unpack_from('<48i', rec, 2 + i * 484 + 4) if x != -1)
    _, groups, _ = svrfmt.epac_read(open(os.path.join(game, 'pac', 'string.pac'), 'rb').read())
    sdb = dict(next(ents for t, ents in groups if t == b'SDB '))[b'0600']
    out = {}
    for _, b in svrfmt.pach_read(sdb):
        u = _unpack(b)
        for k in range(struct.unpack_from('<I', u, 4)[0]):
            off, ln, sid, _ = struct.unpack_from('<4I', u, 8 + 16 * k)
            if sid in ids:
                out.setdefault(u[off:off + ln].split(b'\0')[0].decode('latin1').strip(), sid)
    return out


# ---- the model

def _area(node):
    n = node.lower()
    for k, a in (('yubi', 'hand'), ('_te', 'hand'), ('kote', 'forearm'), ('ninoude', 'upperarm'), ('kata', 'upperarm'),
                 ('sakotsu', 'chest'), ('mune', 'chest'), ('onaka', 'chest'), ('koshi', 'hips'), ('root', 'hips'),
                 ('momo', 'thigh'), ('sune', 'shin'), ('tsumasaki', 'foot'), ('ashi', 'foot')):
        if k in n:
            return a
    return 'head'


def _bundle(raw):
    out = {}
    for k in range(struct.unpack_from('<I', raw, 0)[0]):
        rec = raw[16 + 32 * k:48 + 32 * k]
        size, off = struct.unpack_from('<II', rec, 20)
        out[rec[:16].split(b'\0')[0].decode('latin1').lower()] = raw[off:off + size]
    return out


def pac_attires(path):
    """-> {attire (1-4): (models, textures)} from a model pac's EMD entries"""
    _, groups = ch_tool.epk8_read(open(path, 'rb').read())
    out = {}
    for typ, ents in groups:
        if not typ.startswith(b'EMD'):
            continue
        for name, blob in ents:
            if len(name) != 8 or name[7:8] != b'2' or not name[6:7].isdigit():
                continue
            try:
                kids = svrfmt.pach_read(ch_tool.unpack(blob))
            except ValueError:
                continue
            models, tex = [], {}
            for _, b in kids:
                raw = ch_tool.unpack(b)
                if raw[:4] == b'JBOY':
                    models.append(jboy.read(raw))
                elif ch_tool.is_bundle(raw):
                    tex.update(_bundle(raw))
            out[int(name[6:7]) + 1] = (models, tex)
    return out


def _image(dds):
    import numpy as np
    from PIL import Image
    return np.asarray(Image.open(io.BytesIO(dds)).convert('RGBA'))


def vertical(models):
    """-> f(vertex) giving its height, 0 at the feet and 1 at the head: the axis from the foot-weighted
    vertices to the head-weighted ones (a box's longest side is the arms in a T-pose - PS2 models)"""
    import numpy as np
    feet, heads, pts = [], [], []
    for m in models:
        names = [n['name'] for n in m.nodes]
        for s in m.meshes:
            nb = len(s.weights)
            flat = [x for blk in s.weights for x in blk]
            for k, v in enumerate(s.verts):
                pts.append(v[:3])
                for j in range(nb):
                    b, f = flat[nb * k + j]
                    if f > 0.6 and b[0] < len(s.palette) and 0 <= s.palette[b[0]] - 1 < len(names):
                        a = _area(names[s.palette[b[0]] - 1])
                        if a == 'foot':
                            feet.append(v[:3])
                        elif a == 'head':
                            heads.append(v[:3])
    pts = np.array(pts, np.float64)
    if feet and heads:
        f0, h0 = np.mean(feet, axis=0), np.mean(heads, axis=0)
        axis = int(np.argmax(np.abs(h0 - f0)))
        sign = 1.0 if h0[axis] > f0[axis] else -1.0
        proj = pts[:, axis] * sign
        lo, hi = float(proj.min()), float(proj.max())
    else:  # (no named bones: the box's longest side, the head at its low end as 2011's models)
        lo3, hi3 = pts.min(0), pts.max(0)
        axis = int(np.argmax(hi3 - lo3))
        sign, lo, hi = -1.0, -float(hi3[axis]), -float(lo3[axis])
    span = (hi - lo) or 1.0
    return lambda v: (v[axis] * sign - lo) / span


def channels(models, tex):
    """-> [None | {cov, share{area}, mid, span, rgb}] per mask channel (4 per mask), or None without masks"""
    import numpy as np
    masks = [tex[k] for k in ('mask1', 'mask2', 'mask3') if k in tex]
    if not masks or 'color' not in tex or not models:
        return None
    color = _image(tex['color'])
    y_of = vertical(models)
    # vertices that sample `color`: (mesh, uv, height 0 feet - 1 head, [(area, weight)])
    verts = []
    for m in models:
        names = [n['name'] for n in m.nodes]
        for s in m.meshes:
            tx = dict((n, v) for n, t, v in s.params).get('texDiffuse')
            ti = int.from_bytes(tx[:4], 'big', signed=True) if tx else -1
            if not (0 <= ti < len(m.textures)) or m.textures[ti] != 'color':
                continue
            nb = len(s.weights)
            flat = [x for blk in s.weights for x in blk]  # (jboy.read splits them per block; they are per vertex)
            for k, (u, v) in enumerate(s.uvs):
                inf = []
                for j in range(nb):
                    b, f = flat[nb * k + j]
                    if f > 0 and b[0] < len(s.palette) and 0 <= s.palette[b[0]] - 1 < len(names):
                        inf.append((_area(names[s.palette[b[0]] - 1]), f))
                main = max(inf, key=lambda af: af[1])[0] if inf else 'head'
                verts.append((u % 1.0, v % 1.0, y_of(s.verts[k]), inf, main))
    out = []
    for dds in masks:
        hints = dds[36:40] if dds[32:36] == b'SVRT' else bytes((255,) * 4)  # (masks made here: their groups)
        mk = _image(dds)
        h, w = mk.shape[:2]
        for c in range(4):
            ch = mk[:, :, c].astype(np.float32) / 255
            sel = ch > 0.03
            cov = float(sel.mean())
            if cov < 0.0005:
                out.append(None)
                continue
            wgt = collections.Counter()
            ys, ys_hint = [], []
            hint = GROUP_ORDER[hints[c]] if hints[c] < len(GROUP_ORDER) else None
            for u, v, y, inf, main in verts:
                val = ch[min(h - 1, int(v * h)), min(w - 1, int(u * w))]
                if val <= 0.03:
                    continue
                ys.append(y)
                if hint and _group(main) == hint:  # (the painter's group: another body part sampling the same
                    ys_hint.append(y)              # texels - shared pads - doesn't stretch its height)
                for a, f in inf:
                    wgt[a] += f * val
            if ys_hint:
                ys = ys_hint
            if not ys:
                out.append(None)
                continue
            tot = sum(wgt.values()) or 1.0
            if color.shape[:2] != (h, w):
                from PIL import Image
                ch = np.asarray(Image.fromarray((ch * 255).astype(np.uint8)).resize(color.shape[1::-1])) / 255.0
                sel = ch > 0.03
            core = ch >= 0.6 * float(ch.max())  # (the mask's core: its soft edges blend into the next part)
            rgb = np.median(color[:, :, :3][core].astype(np.float32), axis=0)
            out.append({'cov': cov, 'hint': hint,
                        'share': {a: wgt[a] / tot for a in AREAS},
                        'mid': float(np.median(ys)), 'span': float(np.percentile(ys, 95) - np.percentile(ys, 5)), 'rgb': rgb})
    return out


# ---- naming

def family(c, has_pure_foot):
    s = c['share']
    arms = s['upperarm'] + s['forearm'] + s['hand']
    legs = s['thigh'] + s['shin']
    hint = c.get('hint')
    if hint == 'arms':  # (the painter's group: only the arm areas count)
        a = arms or 1.0
        if s['hand'] / a >= 0.8:
            return 'GLOVES'
        if s['upperarm'] / a >= 0.3 and s['forearm'] / a >= 0.3:
            return 'ELBOW PADS'
        return 'ARMBANDS' if s['upperarm'] / a >= 0.6 else 'WRIST TAPE'
    if hint == 'top':
        return 'SHIRT' if arms >= 0.2 else 'SINGLET'
    if hint == 'waist':
        return 'TRUNKS'
    if hint == 'legs':
        # (pads: a short band at the knee, and small - stock kneepads take 3-4 % of the texture)
        return 'KNEE PADS' if c['span'] < 0.3 and 0.18 <= c['mid'] <= 0.45 and c['cov'] <= 0.045 else 'TIGHTS'
    if hint == 'feet':
        if s['foot'] >= 0.9 and c['mid'] < 0.05 and has_pure_foot == 'soles':
            return 'SOLES'
        return 'KICKPADS' if has_pure_foot and s['shin'] >= 0.5 and c['mid'] >= 0.08 else 'BOOTS'
    if s['head'] >= 0.5:
        return 'MASK'
    if s['chest'] >= 0.25 or (s['chest'] >= 0.15 and c['cov'] > 0.05):
        return 'SHIRT' if arms >= 0.2 else 'SINGLET'
    if arms >= 0.6:
        if s['hand'] >= 0.8:
            return 'GLOVES'
        if s['upperarm'] >= 0.3 and s['forearm'] >= 0.3:
            return 'ELBOW PADS'
        if s['upperarm'] >= 0.6:
            return 'ARMBANDS'
        return 'WRIST TAPE'
    if s['hips'] >= 0.6:
        return 'TRUNKS'
    if legs + s['foot'] + s['hips'] >= 0.6 and c['span'] >= 0.3 and c['mid'] >= 0.15:
        return 'TIGHTS'
    if legs >= 0.6 and c['mid'] >= 0.18:
        return 'KNEE PADS' if c['span'] < 0.3 else 'TIGHTS'
    if s['foot'] + s['shin'] >= 0.5 or c['mid'] < 0.18:
        if s['foot'] >= 0.9 and c['mid'] < 0.05 and has_pure_foot == 'soles':
            return 'SOLES'
        if has_pure_foot and s['shin'] >= 0.5 and c['mid'] >= 0.08:
            return 'KICKPADS'
        return 'BOOTS'
    # (mixed: the strongest body group - a PS2 model's pads share one texture spot for elbows and knees)
    g = max((('MASK', s['head']), ('SHIRT', s['chest']), ('ELBOW PADS', arms), ('TRUNKS', s['hips']),
             ('KNEE PADS' if c['span'] < 0.3 else 'TIGHTS', legs), ('BOOTS', s['foot'])), key=lambda kv: kv[1])
    return g[0]


def name_parts(chans, known):
    """-> [(part 1-12, name, family)] for the used channels"""
    used = [(i, c) for i, c in enumerate(chans) if c and i < 12]
    feet = [c for _, c in used if c['share']['foot'] >= 0.9 and c['mid'] < 0.05]
    pure = 'soles' if len(feet) >= 2 or (feet and any(c['share']['foot'] >= 0.9 and c['mid'] >= 0.05 or
                                                       c['share']['shin'] < 0.5 and c['mid'] < 0.18 and c not in feet
                                                       for _, c in used)) else bool(feet)
    groups = collections.OrderedDict()
    for i, c in used:
        groups.setdefault(family(c, pure), []).append((i, c))
    torso = groups.pop('SHIRT', []) + groups.pop('SINGLET', [])
    if torso:
        big = max(torso, key=lambda ic: ic[1]['cov'])[1]['share']
        groups['SHIRT' if big['upperarm'] + big['forearm'] + big['hand'] >= 0.2 else 'SINGLET'] = torso
    if 'SOLES' in groups and 'BOOTS' not in groups:
        groups['BOOTS'] = groups.pop('SOLES')
    if len(groups.get('SOLES', [])) > 1:  # (one sole; the rest are boots)
        s = sorted(groups['SOLES'], key=lambda ic: ic[1]['mid'])
        groups['SOLES'], groups['BOOTS'] = s[:1], groups.get('BOOTS', []) + s[1:]
    out = []
    taken = set()
    for fam, items in groups.items():
        one, nums, design, dnums = FAMILIES[fam]
        top = max(c['cov'] for _, c in items)
        bases = sorted([ic for ic in items if ic[1]['cov'] >= 0.5 * top], key=lambda ic: ic[0])
        designs = sorted([ic for ic in items if ic[1]['cov'] < 0.5 * top], key=lambda ic: ic[0])
        if not design and not dnums:  # (no design names: all bases)
            bases, designs = sorted(items, key=lambda ic: ic[0]), []
        plan = []
        plan += [one] if len(bases) == 1 else (nums + [one] * len(bases))[:len(bases)]
        if designs:
            pool = ([design] if len(designs) == 1 and design else []) + dnums + ([design] if design else [])
            pool += [pool[-1] if pool else one] * len(designs)
            plan += pool[:len(designs)]
        for (i, c), nm in zip(bases + designs, plan):
            if nm not in known:  # (a name the game doesn't have: the family's plain one)
                nm = one
            if nm in taken and fam in ('BOOTS', 'TIGHTS', 'TRUNKS', 'KNEE PADS', 'SHIRT') and nums:
                nm = next((n for n in nums if n not in taken and n in known), nm)
            taken.add(nm)
            out.append((i + 1, nm, fam, c))
    return sorted(out)


def colour(rgb):
    """the Threads default for the texture's mean colour: brightened as the game's defaults are, at least 0x15"""
    return '%02x%02x%02x' % tuple(max(0x15, min(255, int(round(255 * (float(x) / 255) ** 0.6)))) for x in rgb)


# ---- masks for models without them (ports from SvR 2006-2009 and the PS2 games)
#
# Threads needs the clothes marked per texel: their meshes hold skin too (a legs mesh is trousers and
# knees). The skin's colour is read off the head meshes; a texel of a clothing mesh whose colour is clearly
# not the skin's is cloth. Cloth texels form regions in the texture; each region gets the body area under it
# (the vertices' bone weights) and its colour; regions of one area and colour make one channel, the biggest
# eight channels go in (Mask1, Mask2). The game paints them as it paints its own (added children 0x0d /
# 0x0e, A4R4G4B4, value 11 of 15 as the stock masks).

MASK_LEVEL = 11
GROUP_ORDER = ('arms', 'top', 'waist', 'legs', 'feet')
SKIN_FAR = 60.0      # (RGB distance from the skin's colour: cloth beyond it)
SAME_COLOUR = 45.0   # (regions of one area this close in colour: one channel)
NEAR_SKIN = 80.0     # (a small region this close to the skin's colour: not clothing)
THIN_FILL = 0.25     # (a region filling less of its bounding box: a pattern, not clothing)
MIN_REGION = 0.0015  # (smaller regions: specks, left out)
TATTOO_MAX = 0.02    # (a dark region inside skin smaller than this share of the texture: a tattoo)


def _group(area):
    return {'chest': 'top', 'upperarm': 'arms', 'forearm': 'arms', 'hand': 'arms', 'hips': 'waist',
            'thigh': 'legs', 'shin': 'legs', 'foot': 'feet'}.get(area)


def _label(arr):
    """4-connected regions of a boolean (h, w) array -> (labels int32, count)"""
    import numpy as np
    h, w = arr.shape
    lab = np.zeros((h, w), np.int32)
    parent = [0]

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    for y in range(h):
        row, up = arr[y], lab[y - 1] if y else None
        for x in np.flatnonzero(row):
            left = lab[y, x - 1] if x and row[x - 1] else 0
            top = up[x] if up is not None and arr[y - 1, x] else 0
            if not left and not top:
                parent.append(len(parent))
                lab[y, x] = len(parent) - 1
            else:
                a, b = find(left) if left else 0, find(top) if top else 0
                lab[y, x] = a or b
                if a and b and a != b:
                    parent[max(a, b)] = min(a, b)
    roots = np.array([find(i) for i in range(len(parent))], np.int32)
    lab = roots[lab]
    uniq, lab = np.unique(lab, return_inverse=True)
    return lab.reshape(h, w).astype(np.int32), len(uniq)


def area_raster(models, w, h):
    """-> uint8 (h, w): each colour-texture triangle drawn with its body area (1 + AREAS index), 0 elsewhere"""
    import numpy as np
    from PIL import Image, ImageDraw
    y_of = vertical(models)
    areas = Image.new('L', (w, h), 0)
    draw = ImageDraw.Draw(areas)
    for m in models:
        names = [n['name'] for n in m.nodes]
        for s in m.meshes:
            tx = dict((n, v) for n, t, v in s.params).get('texDiffuse')
            ti = int.from_bytes(tx[:4], 'big', signed=True) if tx else -1
            if not (0 <= ti < len(m.textures)) or m.textures[ti] != 'color':
                continue
            nb = len(s.weights)
            flat = [x for blk in s.weights for x in blk]
            varea = []
            for k in range(len(s.verts)):
                wgt = collections.Counter()
                for j in range(nb):
                    b, f = flat[nb * k + j]
                    if f > 0 and b[0] < len(s.palette) and 0 <= s.palette[b[0]] - 1 < len(names):
                        wgt[_area(names[s.palette[b[0]] - 1])] += f
                a = wgt.most_common(1)[0][0] if wgt else 'head'
                if a == 'shin' and y_of(s.verts[k]) < 0.12:
                    a = 'foot'  # (a boot's top: low on the shin)
                varea.append(AREAS.index(a) + 1)
            for prim, idx in s.strips:
                for k in range(len(idx) - 2):
                    t = idx[k], idx[k + 1], idx[k + 2]
                    if t[0] == t[1] or t[1] == t[2] or t[0] == t[2]:
                        continue
                    lab = collections.Counter(varea[i] for i in t).most_common(1)[0][0]
                    draw.polygon([((s.uvs[i][0] % 1.0) * w, (s.uvs[i][1] % 1.0) * h) for i in t], fill=lab)
    return np.asarray(areas)


def stale_masks(models, tex):
    """Are the attire's masks of no use: empty (the PS2 converter writes blank ones), or another model's (a
    converter's template copied them - most of what they mark lies outside this model's colour triangles)?"""
    import numpy as np
    masks = [_image(tex[k]) for k in ('mask1', 'mask2', 'mask3') if k in tex]
    if not masks or 'color' not in tex:
        return False
    h, w = masks[0].shape[:2]
    covered = area_raster(models, w, h) > 0
    marked = np.zeros((h, w), bool)
    for mk in masks:
        if mk.shape[:2] == (h, w):
            marked |= (mk > 8).any(axis=2)
    n = int(marked.sum())
    return n == 0 or float((marked & covered).sum()) / n < 0.6


def paint_masks(models, tex):
    """-> (channels: [bool (h, w) array], info: [str], groups: [GROUP_ORDER name per channel]) for an attire
    without masks, or ([], [why], [])"""
    import numpy as np
    from PIL import Image, ImageFilter
    if 'color' not in tex or not models:
        return [], ['no colour texture'], []
    color = _image(tex['color'])[:, :, :3].astype(np.float32)
    h, w = color.shape[:2]
    area = area_raster(models, w, h)
    head = area == AREAS.index('head') + 1
    if not head.any():
        return [], ['no head meshes to read the skin colour from'], []
    # the skin: the head's skin-coloured texels (not hair, beard, eyes: mid brightness, a warm hue)
    hc = color[head]
    mx, mn = hc.max(1), hc.min(1)
    warm = (hc[:, 0] >= hc[:, 2]) & (mx > 60) & (mx < 250) & (mx - mn > 12)
    skin = np.median(hc[warm] if warm.sum() > 200 else hc, axis=0)
    dist = np.sqrt(((color - skin) ** 2).sum(-1))
    cloth = (area > 0) & ~head & (dist > SKIN_FAR)
    # specks and seams away, small holes closed (opening then closing, 5 px)
    im = Image.fromarray((cloth * 255).astype(np.uint8))
    im = im.filter(ImageFilter.MinFilter(5)).filter(ImageFilter.MaxFilter(5))
    im = im.filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.MinFilter(5))
    cloth = (np.asarray(im) > 127) & (area > 0) & ~head
    # regions (found at a quarter of the size, then each full-size texel takes its block's region)
    small = cloth.reshape(h // 4, 4, w // 4, 4).any(axis=(1, 3))
    lab, n = _label(small)
    small_used = (area > 0).reshape(h // 4, 4, w // 4, 4).any(axis=(1, 3))
    small_skin = ((area > 0) & (dist <= SKIN_FAR)).reshape(h // 4, 4, w // 4, 4).mean(axis=(1, 3)) > 0.5
    full = np.repeat(np.repeat(lab, 4, 0), 4, 1) * cloth
    groups = {}  # (group, colour cluster) -> [texel mask, texels, rgb]
    thin = 0
    info = ['skin colour %02x%02x%02x, cloth %.1f%% of the texture' % (tuple(int(x) for x in skin) + (100.0 * cloth.mean(),))]
    for r in range(1, n):
        sel = full == r
        cnt = int(sel.sum())
        if cnt < MIN_REGION * h * w:
            continue
        a = np.bincount(area[sel], minlength=len(AREAS) + 1)
        g = _group(AREAS[int(np.argmax(a[1:]))])
        if not g:
            continue
        # (a pattern on the skin - a tattoo - isn't clothing: thin and branching (it fills little of its box), or
        # darker than the skin and surrounded by it (a ring around it is skin where the model uses the texture))
        ys, xs = np.nonzero(lab == r)
        fill = float(len(ys)) / ((ys.max() - ys.min() + 1) * (xs.max() - xs.min() + 1))
        rgb = np.median(color[sel], axis=0)
        if fill < THIN_FILL:
            thin += 1
            continue
        if rgb.max() < 0.8 * skin.max() and cnt < TATTOO_MAX * h * w:  # (a kneepad round the knee is bigger)
            reg = Image.fromarray(((lab == r) * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(5))
            ring = (np.asarray(reg) > 127) & (lab != r) & small_used
            if ring.any() and float((ring & small_skin).sum()) / ring.sum() > 0.8:
                thin += 1
                continue
        key = next((k for k in groups if k[0] == g and np.linalg.norm(groups[k][2] - rgb) < SAME_COLOUR), None)
        if key is None:
            groups[(g, len(groups))] = [sel, cnt, rgb]
        else:
            e = groups[key]
            e[0] = e[0] | sel
            e[2] = (e[2] * e[1] + rgb * cnt) / (e[1] + cnt)
            e[1] += cnt
    if thin:
        info.append('%d pattern(s) on the skin (tattoos?) left out' % thin)
    # (a small region near the skin's colour - chest hair, a shadow - isn't clothing either)
    near = [k for k, e in groups.items() if np.linalg.norm(e[2] - skin) < NEAR_SKIN and e[1] < 0.01 * h * w]
    for k in near:
        del groups[k]
    if near:
        info.append('%d small skin-coloured region(s) left out' % len(near))
    best = sorted(groups.items(), key=lambda kv: -kv[1][1])[:8]
    best.sort(key=lambda kv: (GROUP_ORDER.index(kv[0][0]), -kv[1][1]))
    for (g, _), (sel, cnt, rgb) in best:
        info.append('%s %02x%02x%02x %.1f%%' % ((g,) + tuple(int(x) for x in rgb) + (100.0 * cnt / (h * w),)))
    return [sel for _, (sel, cnt, rgb) in best], info, [g for (g, _), _ in best]


def mask_bundles(chans, head16, groups=()):
    """channels (up to 8 bool arrays) -> {child id: texture bundle}: Mask1 (0x0d), Mask2 (0x0e). Each DDS's
    reserved words (+32) say "SVRT" and each channel's body group (GROUP_ORDER index, 255 none): the names
    come from them (a model's hands may sample its trousers' texels)."""
    import numpy as np
    out = {}
    h, w = chans[0].shape
    for mi in range((len(chans) + 3) // 4):
        px = np.zeros((h, w), np.uint16)
        for c, sel in enumerate(chans[mi * 4:mi * 4 + 4]):
            px |= np.where(sel, MASK_LEVEL << (8, 4, 0, 12)[c], 0).astype(np.uint16)  # (R G B A)
        dds = bytearray(128)
        dds[0:4] = b'DDS '
        struct.pack_into('<7I', dds, 4, 124, 0x1 | 0x2 | 0x4 | 0x1000, h, w, w * 2, 0, 0)
        struct.pack_into('<8I', dds, 76, 32, 0x41, 0, 16, 0x0F00, 0x00F0, 0x000F, 0xF000)
        struct.pack_into('<I', dds, 108, 0x1000)
        hint = [GROUP_ORDER.index(g) if g in GROUP_ORDER else 255 for g in list(groups)[mi * 4:mi * 4 + 4]]
        dds[32:40] = b'SVRT' + bytes((hint + [255] * 4)[:4])
        name = ('Mask%d' % (mi + 1)).encode()
        data = bytes(dds) + px.astype('<u2').tobytes()
        rec = name.ljust(16, b'\0') + b'dds\0' + struct.pack('<II', len(data), 0x30) + b'\0' * 4
        out[0x0d + mi] = struct.pack('<I', 1) + head16[4:16] + rec + data
    return out


def _pack(raw):
    """BPE-compressed (the game unpacks an entry in place: packed must be smaller than raw). svrmod.exe when
    it's there; else the pure-Python pair coder - a mask is mostly the same few 4000-byte blocks, coded once."""
    if os.path.isfile(svrfmt._CLI):
        try:
            return svrfmt.bpe_pack(raw)
        except Exception:  # noqa: BLE001 - the Python coder below
            pass
    cache, out = {}, bytearray()
    for start in range(0, len(raw), 4000):
        chunk = bytes(raw[start:start + 4000])
        code = cache.get(chunk)
        if code is None:
            code = cache[chunk] = svrfmt._bpe_block(chunk)
        out += code
    packed = b'BPE ' + struct.pack('<III', 0x100, len(out), len(raw)) + bytes(out)
    if len(packed) >= len(raw):
        raise ValueError('a mask that does not compress')
    return packed


def _write_epk8(src, entries, out):
    """an EPK8 like `src` (its header) with these (name8, blob) EMD entries (as svr08_char.write_epk8)"""
    header = bytearray(src[:0x800])
    index = bytearray(0x3800)
    struct.pack_into('<4sHH', index, 0, b'EMD ', 4 * len(entries), 0)
    index[6:12] = src[0x806:0x80C]
    body = bytearray()
    p = 12
    for name, blob in entries:
        body += bytes(-len(body) % 0x800)
        index[p:p + 8] = name.ljust(8, b'\0')[:8]
        struct.pack_into('<II', index, p + 8, len(body) // 0x800, (len(blob) + 0xFF) // 0x100)
        body += blob
        p += 16
    body += bytes(-len(body) % 0x800)
    struct.pack_into('<I', header, 4, 12 + 16 * len(entries))
    struct.pack_into('<I', header, 8, len(body))
    tmp = out + '.part'
    with open(tmp, 'wb') as f:
        f.write(bytes(header) + bytes(index) + bytes(body) + bytes(0x800))
    os.replace(tmp, out)


def make_masks(pac, out=None, preview=None, force=False):
    """Masks for every attire of the model pac `pac` that has none -> written to `out` (None: nothing written).
    Returns [(attire, channels, info lines)]."""
    raw = open(pac, 'rb').read()
    _, groups = ch_tool.epk8_read(raw)
    entries = [(n, b) for t, es in groups if t.startswith(b'EMD') for n, b in es if n.strip(b'\0')]
    if len(entries) != sum(1 for t, es in groups for n, b in es if n.strip(b'\0')):
        raise ValueError('a pac with more than EMD entries: not rewritten')
    done, changed = [], False
    for k, (name, blob) in enumerate(entries):
        if len(name) != 8 or name[7:8] != b'2' or not name[6:7].isdigit():
            continue
        kids = svrfmt.pach_read(ch_tool.unpack(blob))
        models, tex, colour_head = [], {}, None
        for i, b in kids:
            r = ch_tool.unpack(b)
            if r[:4] == b'JBOY':
                models.append(jboy.read(r))
            elif ch_tool.is_bundle(r):
                tex.update(_bundle(r))
                if i == 0x0c:
                    colour_head = r[:16]
        stale = False
        if any(t.startswith('mask') for t in tex):
            if not force and not stale_masks(models, tex):
                continue
            stale = True  # (another model's masks: replaced)
            tex = {t: v for t, v in tex.items() if not t.startswith('mask')}
            kids = [(i, b) for i, b in kids if i not in (0x0d, 0x0e, 0x0f)]
        chans, info, groups = paint_masks(models, tex)
        if stale:
            info.insert(0, "its masks were empty or another model's (not its UVs): made anew")
        attire = int(name[6:7]) + 1
        done.append((attire, len(chans), info))
        if not chans:
            continue
        add = mask_bundles(chans, colour_head or struct.pack('<IIII', 1, 0x100, 0, 0x10), groups)
        new = [(i, b) for i, b in kids if i not in add]
        new += [(i, _pack(b)) for i, b in add.items()]
        new.sort(key=lambda ib: ib[0])
        entries[k] = (name, svrfmt.pach_write(new))
        changed = True
        if preview:
            _preview(tex, chans, preview.replace('.png', '_%d.png' % attire))
    if changed and out:
        _write_epk8(raw, entries, out)
    return done


def _preview(tex, chans, path):
    import numpy as np
    from PIL import Image
    col = _image(tex['color'])[:, :, :3].astype(np.float32)
    pal = np.array([(255, 0, 0), (0, 255, 0), (0, 96, 255), (255, 255, 0), (255, 0, 255), (0, 255, 255),
                    (255, 128, 0), (160, 0, 255)], np.float32)
    for c, sel in enumerate(chans):
        col[sel] = col[sel] * 0.35 + pal[c] * 0.65
    Image.fromarray(col.astype(np.uint8)).resize((col.shape[1] // 2, col.shape[0] // 2)).save(path)


# ---- the mod

def manifest(folder):
    out = {}
    p = os.path.join(folder, 'manifest.txt')
    if os.path.exists(p):
        for line in open(p, encoding='utf-8', errors='replace'):
            if '=' in line and not line.lstrip().startswith('#'):
                k, v = line.split('=', 1)
                out[k.strip().lower()] = v.strip()
    return out


def mod_attires(folder):
    """-> {attire: (models, textures)}: ch.pac's attires, attireN= pacs' attire 1 in their place"""
    out = pac_attires(os.path.join(folder, 'ch.pac'))
    man = manifest(folder)
    for n in (2, 3, 4):
        f = man.get('attire%d' % n)
        if f and os.path.exists(os.path.join(folder, f)):
            other = pac_attires(os.path.join(folder, f))
            if 1 in other:
                out[n] = other[1]
    return out


def lacks_masks(pac):
    """Has the model pac an attire without Mask1 (texture bundle names only - nothing decoded)?"""
    _, groups = ch_tool.epk8_read(open(pac, 'rb').read())
    for typ, ents in groups:
        for name, blob in ents:
            if not typ.startswith(b'EMD') or len(name) != 8 or name[7:8] != b'2' or not name[6:7].isdigit():
                continue
            try:
                kids = svrfmt.pach_read(ch_tool.unpack(blob))
            except ValueError:
                continue
            names = []
            for _, b in kids:
                if b[:4] == b'BPE ' and len(b) > 64 or ch_tool.is_bundle(b):
                    raw = ch_tool.unpack(b)
                    if ch_tool.is_bundle(raw):
                        names += [n.lower() for n in ch_tool.bundle_names(raw)]
            if 'color' in names and not any(n.startswith('mask') for n in names):
                return True
    return False


def add_masks(folder, write=True, preview=None, backup=True, force=False):
    """Masks into the mod's model pacs (ch.pac, attireN= pacs) where an attire has none; each pac's first
    version kept in <mod>/.threads_backup (backup=False: a converter's fresh mod). -> [(file, attire, channels, info)]"""
    import shutil
    man = manifest(folder)
    files = ['ch.pac'] + [man['attire%d' % n] for n in (2, 3, 4) if man.get('attire%d' % n)]
    out = []
    for f in files:
        p = os.path.join(folder, f)
        if not os.path.exists(p):
            continue
        shown = preview and preview.replace('.png', '_' + os.path.splitext(f)[0] + '.png')
        bak, made_bak = os.path.join(folder, '.threads_backup', f), False
        if write and backup and not os.path.exists(bak):
            os.makedirs(os.path.dirname(bak), exist_ok=True)
            shutil.copyfile(p, bak)
            made_bak = True
        plan = make_masks(p, p if write else None, shown, force)  # (no mask missing nor another model's: unchanged)
        if made_bak and not any(n for _, n, _ in plan):
            os.remove(bak)
            if not os.listdir(os.path.dirname(bak)):
                os.rmdir(os.path.dirname(bak))
        out += [(f, a, n, info) for a, n, info in plan]
    return out


def build(folder, game):
    known = game_names(game)
    result = []
    for attire, (models, tex) in sorted(mod_attires(folder).items()):
        chans = channels(models, tex)
        if not chans:
            continue
        parts = [{'part': p, 'name': nm, 'id': known[nm], 'colour': colour(c['rgb']), 'area': fam.lower()}
                 for p, nm, fam, c in name_parts(chans, known)]
        if parts:
            result.append({'attire': attire, 'parts': parts})
    return result


def write(folder, attires):
    lines = ['# Superstar Threads parts (threads_parts.py %d): attire.part = name id, default colour  # name' % FORMAT,
             '# part p paints channel (p-1)%4 of the attire\'s Mask1/2/3 (R G B A). Edit freely; the game checks it.',
             'format=%d' % FORMAT]
    for a in attires:
        for p in a['parts']:
            lines.append('%d.%d=%d,%s  # %s' % (a['attire'], p['part'], p['id'], p['colour'], p['name']))
    path = os.path.join(folder, 'threads.txt')
    tmp = path + '.part'
    with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    os.replace(tmp, path)
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('folder', nargs='?')
    ap.add_argument('--game', default=GAME)
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--names', action='store_true')
    ap.add_argument('--make-masks', action='store_true')
    ap.add_argument('--remake-masks', action='store_true', help='--make-masks, replacing masks it finds too')
    ap.add_argument('--preview', help='with --make-masks: <file.png>, the masks over the colour texture per attire')
    a = ap.parse_args()
    if a.names:
        print(json.dumps(sorted(game_names(a.game).items())))
        return 0
    if not a.folder or not os.path.exists(os.path.join(a.folder, 'ch.pac')):
        msg = 'no ch.pac in %s' % a.folder
        print(json.dumps({'ok': False, 'message': msg}) if a.json else msg)
        return 2
    masks = []
    a.make_masks = a.make_masks or a.remake_masks
    if a.make_masks:
        masks = add_masks(a.folder, write=not a.dry_run, preview=a.preview, force=a.remake_masks)
        if not a.json:
            for f, attire, n, info in masks:
                print('%s attire %d: %d mask channel(s) - %s' % (f, attire, n, '; '.join(info)))
    attires = build(a.folder, a.game) if not (a.make_masks and a.dry_run) else []
    made = [{'pac': f, 'attire': at, 'channels': n, 'info': info} for f, at, n, info in masks]
    if not attires:
        msg = ('masks shown (dry run)' if a.make_masks and a.dry_run else
               'its model has no Threads masks (Mask1/Mask2): no parts to paint (--make-masks makes them)')
        print(json.dumps({'ok': False, 'attires': [], 'masks': made, 'message': msg}) if a.json else msg)
        return 0 if a.make_masks and a.dry_run else 3
    path = None if a.dry_run else write(a.folder, attires)
    if a.json:
        print(json.dumps({'ok': True, 'file': path, 'attires': attires, 'masks': made,
                          'message': '%d part(s) in %d attire(s)' % (sum(len(x['parts']) for x in attires), len(attires))}))
    else:
        for x in attires:
            print('attire %d: %s' % (x['attire'], ', '.join('%d %s #%s' % (p['part'], p['name'], p['colour']) for p in x['parts'])))
        print(path or '(dry run)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
