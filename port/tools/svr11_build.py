"""Neutral model -> SvR 2011 character pac (ch.pac): the builder the Mod Maker's FBX import calls.

    py svr11_build.py <model.json> <out ch.pac> [report.json]
    py svr11_build.py --gear-from <donor ch.pac>,<attire> --onto <target ch.pac>,<attire> <out ch.pac> [report.json]
                      (entrance gear moved from one character onto another: gear_port.py)
    py svr11_build.py --gear-json <gear.json> --onto <target ch.pac>,<attire> <out ch.pac> [report.json]
                      (gear meshes from the FBX path onto an existing model's attire: gear_port.py)

The caller (FBX reader + tools/rig_maps.json) has already retargeted the meshes to the 2011 bind
skeleton (tools/svr11_skeleton.json: 10 cm units, -Y up, -Z front, pelvis origin, A-pose) and
named the weights by 2011 bones. model.json:

    {"version": 1,
     "template": "<a stock 2011 chNNN.pac: node frames, shader parameters, face animation, cube map>",
     "portrait": "<256 x 256 png / dds, optional>",
     "joints":   {"<2011 bone>": [x, y, z], ...},         optional: world positions that move those
                                                         nodes to the model's proportions (translation only)
     "attires": [{
        "name": "<optional; only '<template id><attire digit>2' is accepted (the entry name the game looks up),
                 anything else - a display name - is ignored with a warning>",
        "hair_chains": [[[x, y, z], ...] | {"parent": "<2011 bone>", "joints": [...]}, ...],   optional: moving hair
                                  (or cloth: a chain hanging from that bone - default atama) - per strand its joints, root (on the
                                  scalp) to tip, 2011 world space, at least 4: built as kami_b00, kami_b00_00 ...
                                  under atama (one segment length a chain, each bone aimed at the next; at most
                                  16), weights may name them (the tip's go to the node before), 0x3C stock-like,
                                  0x3D the template's (docs/HAIR_PHYSICS.md)
        "entrance_gear": {...},   optional: the same attire wearing its entrance gear (a coat, hat, shirt) -
                                  "meshes" / "textures" as below, plus "extra_bones": [{"name", "parent",
                                  "position": [x, y, z]}] (appended; weights may name them) -> entry
                                  "<id><attire - 1>4", which the game wears for the entrance
        "textures": {"color": "<png>", "normal": "<png>", "mask": "<png, optional>"},
        "meshes": [{
            "name": "...", "kind": "skin" | "eye" | "hair" | "lash",
            "positions": [[x, y, z], ...], "normals": [[x, y, z], ...], "uvs": [[u, v], ...],  (v down)
            "bones": ["l_te", ...],
            "weights": [[[bone index, weight], ...], ...],   per vertex, any count (top 4 kept, renormalised)
            "triangles": [[a, b, c], ...],
            "material": {                                   optional, per mesh:
                "specular": 0.48,        shine (g_fSpecularLev: stock 0-100, skin ~0.5), default the template's; the colour alpha is the per-texel part
                "normal": "<png>",       this mesh's normal map (same as textures.normal)
                "tint": [r, g, b] | "#rrggbb",   the mesh's colour multiplier (g_f4MatDifCol; stock 0.36-1.0)
                "cutout": true,          see-through where the colour png's alpha is low (render flags 0x110, as
                                         stock hair cards); otherwise colour alpha = specular level
                "double_sided": true,    back faces drawn (render flag 0x2 cleared, as stock double-sided parts)
                "specular_alpha": "texture" | "flat" | "brightness"},   the colour alpha (specular modulation):
                                         the png's own / 255 (uniform) / from the brightness; default the png's
                                         own when it has one, else brightness; ignored with "cutout" (opacity)
            "textures": {"color": "<png>", "normal": "<png>"}   optional: per-mesh sheets, packed into the
                                                               attire's one colour / normal sheet (UVs remapped)
        }, ...]}]}

Paths are relative to model.json. What the builder does (the rules in svr11_skeleton.json "mesh_rules"):
palettes of <= 20 bones (meshes split as needed, <= 65535 vertices each), weights vertex-major in 1-4
blocks, triangle strips joined with degenerates, vertex colour FFFFFFFF, the template's parameter blocks
per kind (skin yBumpMapSS, eye yReflect, hair, lash yDefault), textures to DXT5 with full mips (colour
alpha = specular level unless the png has one; Mk from the mask or flat), the template's other
children and blood overlays, the 192-page vertex budget (blood overlays dropped first, else an
error), blank damage masks (Mask1-3), face pivots moved onto the model's eyes when they're off by more
than 0.15, EPK8 with one EMD group. (The face pivots and the NaN check follow findings first documented by
robert32's SvR Port Doctor.)

stdout: JSON report {ok, out, attires: [{name, meshes, pages, dropped}], warnings, error}.
Exit 0 ok, 2 bad input, 3 failed (over budget...).
"""
import argparse
import io
import json
import math
import os
import struct
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import jboy  # noqa: E402
import model_budget  # noqa: E402
import w13_char as W  # noqa: E402

MAX_PAL = 20
MAX_VERTS = 65535
COLOR_SIZE = (2048, 1024)     # the stock colour sheet
NORMAL_SIZE = (1024, 1024)
MK_SIZE = (512, 256)
SKIN_SLOTS = {'texDiffuse': 'color', 'texNormal': 'normal', 'texOcclusion': 'mk', 'texTilingMap': 'noise'}
EYE_SLOTS = {'texDiffuse': 'color', 'texCubeRefction': 'cubemap', 'texRefctionReg': 'color', 'texOcclusion': 'mk'}
LASH_SLOTS = {'texDiffuse': 'color'}
TEX_BODY = ['color', 'normal', 'mk', 'noise', 'cubemap']


class BuildError(Exception):
    pass


# ---------------------------------------------------------------- geometry
def stripify(tris):
    """triangle list -> one degenerate-joined strip (strip_tris convention: odd triangles flipped)"""
    tris = [tuple(t) for t in tris if len(set(t)) == 3]
    edge = {}
    for i, (a, b, c) in enumerate(tris):
        for e in ((a, b), (b, c), (c, a)):
            edge.setdefault(e, []).append(i)
    used = [False] * len(tris)

    def cyc(t):
        return {t, (t[1], t[2], t[0]), (t[2], t[0], t[1])}

    def grow(first, start):
        s = list(start)
        took = [first]
        while True:
            a, b = s[-2], s[-1]
            k = len(s) - 2           # the new triangle's index in the strip
            nxt = None
            for e in ((a, b), (b, a)):
                for j in edge.get((e[1], e[0]), []) + edge.get(e, []):
                    if used[j] or j in took:
                        continue
                    d = next(v for v in tris[j] if v not in (a, b))
                    drawn = (a, b, d) if k % 2 == 0 else (b, a, d)
                    if drawn in cyc(tris[j]):
                        nxt = (j, d)
                        break
                if nxt:
                    break
            if not nxt:
                return s, took
            took.append(nxt[0])
            s.append(nxt[1])

    strips = []
    for i, t in enumerate(tris):
        if used[i]:
            continue
        best = None
        for rot in (t, (t[1], t[2], t[0]), (t[2], t[0], t[1])):
            s, took = grow(i, rot)
            if best is None or len(s) > len(best[0]):
                best = (s, took)
        used[i] = True
        for j in best[1]:
            used[j] = True
        strips.append(best[0])
    out = []
    for s in strips:
        if out:
            out += [out[-1], s[0]]
            if len(out) % 2:          # keep the next strip's first triangle on an even index
                out.append(s[0])
        out += s
    return out


def split_by_palette(tris, vbones):
    """-> [list of triangle indices], each needing <= 20 bones and <= 65535 vertices"""
    order = sorted(range(len(tris)), key=lambda i: (min(vbones[tris[i][0]] or (0,)), i))
    groups, cur, bones, verts = [], [], set(), set()
    for i in order:
        tb = set(vbones[tris[i][0]]) | set(vbones[tris[i][1]]) | set(vbones[tris[i][2]])
        tv = set(tris[i])
        if cur and (len(bones | tb) > MAX_PAL or len(verts | tv) > MAX_VERTS):
            groups.append(cur)
            cur, bones, verts = [], set(), set()
        cur.append(i)
        bones |= tb
        verts |= tv
    if cur:
        groups.append(cur)
    return groups


# ---------------------------------------------------------------- textures
def load_png(path):
    return Image.open(path).convert('RGBA')


def tint_rgb(t):
    """a tint: [r, g, b] (0-1, or 0-255 when any is over 1) or '#rrggbb' -> 3 multipliers 0-1"""
    if isinstance(t, str):
        h = t.lstrip('#')
        return tuple(int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4))
    v = [float(x) for x in list(t)[:3]]
    s = 255.0 if max(v) > 1.0 else 1.0
    return tuple(min(1.0, max(0.0, x / s)) for x in v)


def has_alpha(im):
    return im.getextrema()[3][0] < 255


def spec_alpha(im):
    """2011 colour alpha = specular level: a mid value from the brightness (w13_char.add_alpha)"""
    rgb = im.convert('RGB')
    out = rgb.copy()
    out.putalpha(rgb.convert('L').point(lambda v: 40 + v // 3))
    return out


class Atlas:
    """per-mesh sheets -> one sheet: a grid of equal cells (UVs must lie in 0..1)"""

    def __init__(self, keys):
        self.keys = list(dict.fromkeys(keys))
        n = max(1, len(self.keys))
        self.cols = int(math.ceil(math.sqrt(n)))
        self.rows = int(math.ceil(n / self.cols))

    def cell(self, key):
        i = self.keys.index(key)
        return i % self.cols, i // self.cols

    def uv(self, key, u, v):
        if len(self.keys) <= 1:
            return u, v
        cx, cy = self.cell(key)
        return (cx + min(1.0, max(0.0, u))) / self.cols, (cy + min(1.0, max(0.0, v))) / self.rows

    def sheet(self, images, size, fill):
        if len(self.keys) <= 1:
            im = images.get(self.keys[0]) if self.keys else None
            return W.resize_rgba(im or Image.new('RGBA', size, fill), size, Image.LANCZOS)
        out = Image.new('RGBA', size, fill)
        cw, ch = size[0] // self.cols, size[1] // self.rows
        for k in self.keys:
            im = images.get(k)
            if im is None:
                continue
            cx, cy = self.cell(k)
            out.paste(W.resize_rgba(im, (cw, ch), Image.LANCZOS), (cx * cw, cy * ch))
        return out


# ---------------------------------------------------------------- build
class Builder:
    def __init__(self, spec, base):
        self.spec, self.base = spec, base
        self.warnings = []
        tp = self.path(spec['template'])
        portrait = spec.get('portrait')
        self.conv = W.Conv(tp)
        self.template = tp
        if portrait:
            p = self.path(portrait)
            self.portrait = open(p, 'rb').read() if p.lower().endswith('.dds') else \
                W.dds_encode(W.resize_rgba(load_png(p), (256, 256), Image.LANCZOS), 'DXT5')
        else:
            self.portrait = None
        self.nodes = self.posed_nodes(spec.get('joints') or {})
        self.index = {n['name']: i for i, n in enumerate(self.nodes)}

    def path(self, p):
        return p if os.path.isabs(p) else os.path.join(self.base, p)

    def posed_nodes(self, joints):
        """the template's node table with the given world positions (w13_char.Conv.nodes_for)"""
        c = self.conv
        unknown = [j for j in joints if j not in c.tindex]
        if unknown:
            self.warnings.append('joints not in the template, ignored: %s' % ', '.join(unknown[:10]))
        nodes = [dict(nd) for nd in c.tnodes]
        Wn = []
        for j, nd in enumerate(nodes):
            fr = c.tframe[j]
            p = fr['parent'] if j else -1
            if nd['name'] in joints and p >= 0:
                Rp, Pp = Wn[p][:3, :3], Wn[p][:3, 3]
                nd['t'] = tuple(float(x) for x in Rp.T @ (np.array(joints[nd['name']], float) - Pp))
            M = np.eye(4)
            M[:3, :3] = W.rot(fr['r'])
            M[:3, 3] = nd['t']
            Wn.append(M if p < 0 else Wn[p] @ M)
        return nodes

    def template_mesh(self, kind):
        c = self.conv
        return {'skin': (c.p_skin, SKIN_SLOTS), 'eye': (c.p_eye, EYE_SLOTS), 'hair': (c.p_hair, SKIN_SLOTS),
                'lash': (c.p_lash, LASH_SLOTS)}[kind]

    def jmesh(self, m, kind, tri_idx, uvmap, textures):
        """one 2011 mesh from the triangles tri_idx of neutral mesh m"""
        tmpl, slots = self.template_mesh(kind)
        tris = [m['triangles'][i] for i in tri_idx]
        used = sorted({v for t in tris for v in t})
        remap = {v: k for k, v in enumerate(used)}
        pal = sorted({b for v in used for b, _ in m['_w'][v]})
        slot = {b: k for k, b in enumerate(pal)}
        nw = max(1, max(len(m['_w'][v]) for v in used))
        s = jboy.Mesh()
        s.raw = bytes(tmpl.raw)
        s.u74 = 0
        s.shader = tmpl.shader
        s.vfmt = tmpl.vfmt
        s.palette = [b + 1 for b in pal]
        P, N = m['positions'], m['normals']
        s.verts = [(float(P[v][0]), float(P[v][1]), float(P[v][2]), float(N[v][0]), float(N[v][1]), float(N[v][2]), 0xFFFFFFFF)
                   for v in used]
        s.uvs = [uvmap(m['uvs'][v]) for v in used]
        flat = []
        for v in used:
            ws = m['_w'][v]
            flat += [((slot[b], 0, 0, 0), w) for b, w in ws] + [((255, 0, 0, 0), 0.0)] * (nw - len(ws))
        vc = len(used)
        s.weights = [flat[j * vc:(j + 1) * vc] for j in range(nw)]   # (vertex-major data, jboy's block view)
        s.strips = [(6, stripify([tuple(remap[v] for v in t) for t in tris]))]
        s.u04 = 1
        mat = m.get('material') or {}
        s.params = []
        for name, typ, val in tmpl.params:
            if typ == 0x0f and struct.unpack('>i', val[:4])[0] >= 0:   # (a slot the template leaves unbound stays so)
                want = slots.get(name)
                k = textures.index(want) if want in textures else -1
                val = struct.pack('>i', k) + val[4:]
            if name == 'g_fSweatSPA' and mat.get('cutout'):   # (a cut-out: off the skin shader, which never
                val = struct.pack('>f', 0.0) + val[4:]          # outputs the colour alpha - SPA 0 cuts, in game)
            if name == 'g_fSpecularLev' and mat.get('specular') is not None:   # (shine: stock 0-100, skin ~0.5)
                val = struct.pack('>f', min(100.0, max(0.0, float(mat['specular'])))) + val[4:]
            if name == 'g_f4MatDifCol' and mat.get('tint') and len(val) >= 16:   # (tint: the native colour multiplier)
                rgba = list(struct.unpack('>4f', val[:16]))
                for i, x in enumerate(tint_rgb(mat['tint'])):
                    rgba[i] *= x
                val = struct.pack('>4f', *rgba) + val[16:]
            s.params.append((name, typ, val))
        A = np.array([v[:3] for v in s.verts])
        cen = (A.min(0) + A.max(0)) / 2
        r = float(np.sqrt(((A - cen) ** 2).sum(1)).max())
        md = bytearray(s.raw)
        # render flags (+0x88): 0x2 back faces culled (stock skin 0x6; double-sided parts 0x4), 0x10 | 0x100 the
        # colour alpha cuts out / blends (stock hair cards and lashes 0x114)
        fl = struct.unpack_from('>I', md, 0x88)[0]
        if mat.get('double_sided') is not None:
            fl = (fl & ~0x2) if mat['double_sided'] else (fl | 0x2)
        if mat.get('cutout') is not None:
            fl = (fl | 0x110) if mat['cutout'] else (fl & ~0x110)
        struct.pack_into('>I', md, 0x88, fl)
        struct.pack_into('>I', md, 0x8C, tmpl.vfmt)
        struct.pack_into('>4f', md, 0xA4, float(cen[0]), float(cen[1]), float(cen[2]), r)
        s.raw = bytes(md)
        return s

    def face_pivots(self, meshes):
        """Eye / lid / brow / mouth pivots onto the model's eyes: the eyeball vertices (on l_eye / r_eye) sit in front
        of their bone by the template's own offset; when the model's differ by more than 0.15 the bones under atama
        move by the difference (the mesh stays). Otherwise blinking pushes the lids forward and the eyes look like
        they pop out. (Finding first documented by robert32's SvR Port Doctor.)"""
        c = self.conv
        def worlds(nodes):
            Wn = []
            for j, nd in enumerate(nodes):
                if j < len(c.tframe):
                    fr = c.tframe[j]
                    p, r = (fr['parent'] if j else -1), fr['r']
                else:   # (appended: extra bones / hair chains - their own true r and parent)
                    p = nd['u48'][0]
                    r = (nd['r'][1], nd['r'][2], struct.unpack('>f', struct.pack('>I', nd['u40']))[0])
                M = np.eye(4)
                M[:3, :3] = W.rot(r)
                M[:3, 3] = nd['t']
                Wn.append(M if p < 0 else Wn[p] @ M)
            return Wn

        def eye_centres(verts_by_bone):
            return {b: np.mean(P, 0) for b, P in verts_by_bone.items() if P}
        sides = [self.index[n] for n in ('l_eye', 'r_eye') if n in self.index]
        if 'atama' not in self.index or not sides:
            return
        mine = {b: [] for b in sides}
        for m in meshes:
            if m.get('kind') != 'eye':
                continue
            for v, ws in enumerate(m['_w']):
                b = max(ws, key=lambda x: x[1])[0]
                if b in mine:
                    mine[b].append(m['positions'][v])
        tmpl = {b: [] for b in sides}
        for s in c.body.meshes:
            if s.shader != 'yReflect':
                continue
            nw = len(s.weights)
            flat = [e for blk in s.weights for e in blk]
            for v in range(len(s.verts)):
                e = max(flat[v * nw:(v + 1) * nw], key=lambda x: x[1])
                b = s.palette[e[0][0]] - 1 if e[0][0] < len(s.palette) else -1
                if b in tmpl:
                    tmpl[b].append(s.verts[v][:3])
        Wm, Wt = worlds(self.nodes), worlds(c.tnodes)
        cm, ct = eye_centres(mine), eye_centres(tmpl)
        d = [(cm[b] - Wm[b][:3, 3]) - (ct[b] - Wt[b][:3, 3]) for b in sides if b in cm and b in ct]
        if not d:
            return
        delta = np.mean(d, 0)
        if np.linalg.norm(delta) <= 0.15:
            return
        a = self.index['atama']
        Ra = Wm[a][:3, :3]
        for j, nd in enumerate(self.nodes):
            if c.tframe[j]['parent'] == a and j:
                nd['t'] = tuple(float(x) for x in np.array(nd['t']) + Ra.T @ delta)
        self.warnings.append('face pivots moved by (%.2f, %.2f, %.2f) onto the model eyes' % tuple(delta))

    def weights(self, m):
        """bone names -> node indices; top 4 per vertex, renormalised"""
        bidx = []
        for b in m['bones']:
            if b not in self.index:
                self.warnings.append('%s: bone %s is not in the template, its weights go to root' % (m.get('name'), b))
            bidx.append(self.index.get(b, self.index.get('root', 0)))
        out = []
        for vw in m['weights']:
            acc = {}
            for bi, w in vw:
                if w > 0:
                    acc[bidx[int(bi)]] = acc.get(bidx[int(bi)], 0.0) + float(w)
            top = sorted(acc.items(), key=lambda x: -x[1])[:4]
            tot = sum(w for _, w in top)
            out.append([(b, w / tot) for b, w in top] if tot > 0 else [(self.index.get('root', 0), 1.0)])
        return out

    def skin_shader(self, m):
        """does the game draw this mesh with its skin shader? (ModMakerUIRework, from the converted shaders and a frame
        dump: a yBumpMapSS mesh that is culled (flag 0x2), binds a tiling map and has g_fSweatSPA != 0.) The skin shader
        takes the shine mask from the NORMAL map's alpha (the colour alpha is only the sweat shine); every other one
        from the colour alpha. A cut-out (flags 0x110) keeps its alpha as opacity in the colour."""
        kind = m.get('kind', 'skin')
        if kind not in ('skin', 'hair'):
            return False
        tmpl = self.template_mesh(kind)[0]
        if tmpl.shader != 'yBumpMapSS':
            return False
        mat = m.get('material') or {}
        fl = struct.unpack_from('>I', tmpl.raw, 0x88)[0]
        if mat.get('double_sided') is not None:
            fl = (fl & ~0x2) if mat['double_sided'] else (fl | 0x2)
        if mat.get('cutout') is not None:
            fl = (fl | 0x110) if mat['cutout'] else (fl & ~0x110)
        p = {n: v for n, t, v in tmpl.params}
        tiling = 'texTilingMap' in p and struct.unpack('>i', p['texTilingMap'][:4])[0] >= 0
        spa = 'g_fSweatSPA' in p and struct.unpack('>f', p['g_fSweatSPA'][:4])[0] != 0
        return bool(fl & 0x2) and not (fl & 0x110) and tiling and spa

    def attire(self, a, entry_name):
        meshes = a['meshes']
        for m in meshes:
            n = len(m['positions'])
            for key in ('normals', 'uvs', 'weights'):
                if len(m.get(key, [])) != n:
                    raise BuildError('%s: %d positions but %d %s' % (m.get('name'), n, len(m.get(key, [])), key))
            for key in ('positions', 'normals', 'uvs'):
                if not np.isfinite(np.asarray(m[key], float)).all():
                    raise BuildError('%s: %s has NaN / infinite values' % (m.get('name'), key))
            if m.get('kind', 'skin') not in ('skin', 'eye', 'hair', 'lash'):
                raise BuildError('%s: kind %s' % (m.get('name'), m.get('kind')))
            m['_w'] = self.weights(m)
        if not getattr(self, '_faced', False):
            self._faced = True
            self.face_pivots(meshes)
        # sheets: the attire's own, or per-mesh ones packed into one
        at = a.get('textures', {})
        def mat(m):
            return m.get('material') or {}

        def norm_of(m):
            return mat(m).get('normal') or (m.get('textures') or {}).get('normal') or at.get('normal')

        def col_key(m):   # (one atlas cell per colour png + normal png + alpha mode + shader; the tint is a parameter)
            c = (m.get('textures') or {}).get('color') or at.get('color') or ''
            if not c:
                return ''
            return '%s|%s|%s|%s' % (c, norm_of(m) or '', alpha_mode(m), 'S' if self.skin_shader(m) else '')

        def alpha_mode(m):   # (what the cell's colour alpha means: cut-out opacity, or how specular is modulated)
            if mat(m).get('cutout'):
                return 'texture'
            return mat(m).get('specular_alpha') or ''
        atlas = Atlas([col_key(m) for m in meshes if m.get('kind', 'skin') != 'lash'] or [''])
        for m in meshes:
            u = [uv for uv in m['uvs']]
            if atlas.keys and len(atlas.keys) > 1 and any(not (-0.001 <= x <= 1.001) for uv in u for x in uv):
                self.warnings.append('%s: UVs outside 0..1 are clamped in the packed sheet' % m.get('name'))
        colors, normals = {}, {}
        for m in meshes:
            k = col_key(m)
            if k and k not in colors:
                im = load_png(self.path(k.split('|')[0]))
                mode = alpha_mode(m)
                nk = norm_of(m)
                nim = load_png(self.path(nk)) if nk else None
                if self.skin_shader(m):
                    # (the skin shader: the shine mask is the normal map's alpha - the normal png's own when it has
                    # one and nothing else is asked, else the colour png's alpha, else the brightness; flat = 255.
                    # The colour alpha is then only the sweat shine: stock-like 0.3)
                    # (default without a normal alpha: the brightness, NOT the colour alpha - on a 2011 export that is
                    # the sweat mask, which made white specks in game; "texture" asks for the colour alpha)
                    if mode == 'flat':
                        mask = Image.new('L', im.size, 255)
                    elif mode == 'texture' and has_alpha(im):
                        mask = im.split()[3]
                    elif mode in ('', 'texture') and nim is not None and has_alpha(nim):
                        mask = nim.split()[3]
                    else:
                        mask = spec_alpha(im).split()[3]
                    base = nim if nim is not None else Image.new('RGBA', im.size, (128, 128, 255, 255))
                    base = base.convert('RGBA').copy()
                    base.putalpha(mask.resize(base.size, Image.LANCZOS))
                    normals[k] = base
                    im = im.convert('RGB').convert('RGBA')
                    im.putalpha(77)
                    colors[k] = im
                    continue
                # (other shaders: the colour alpha is the shine mask - the png's own when it has one (a cut-out's
                # opacity, or a level the caller painted), else from the brightness; flat = 255)
                if mode == 'flat':
                    im = im.copy()
                    im.putalpha(255)
                elif mode == 'brightness':
                    im = spec_alpha(im)
                elif mode != 'texture' and not has_alpha(im):
                    im = spec_alpha(im)
                colors[k] = im
                if nim is not None:
                    normals[k] = nim
        color = atlas.sheet(colors, COLOR_SIZE, (128, 128, 128, 255))
        if not colors:
            color = spec_alpha(color)
        normal = atlas.sheet(normals, NORMAL_SIZE, (128, 128, 255, 255))
        if at.get('mask'):
            mk = W.resize_rgba(load_png(self.path(at['mask'])), MK_SIZE, Image.LANCZOS)
            r, g, b, al = mk.split()
            mk = Image.merge('RGBA', (Image.new('L', mk.size, 0), g, b, al))
        else:
            mk = Image.new('RGBA', MK_SIZE, (0, 230, 230, 255))   # (stock Mk averages: no bruises, light occlusion)

        def uvmap_for(m):
            k = col_key(m)
            return lambda uv: atlas.uv(k, float(uv[0]), float(uv[1]))

        kids = {}
        for key, kinds, texs in ((0, ('skin', 'eye'), TEX_BODY), (0x2710, ('hair',), TEX_BODY), (0x4e20, ('lash',), ['color'])):
            sel = [m for m in meshes if m.get('kind', 'skin') in kinds]
            if not sel and key:
                continue
            tm = {0: self.conv.body, 0x2710: self.conv.hair or self.conv.body, 0x4e20: self.conv.lash}[key]
            model = jboy.Model()
            model.header, model.name, model.group = tm.header, tm.name, tm.group
            model.name_tail = getattr(tm, 'name_tail', b'')
            model.textures = texs
            model.nodes = self.nodes
            model.meshes = []
            for m in sel:
                vb = [tuple(b for b, _ in ws) for ws in m['_w']]
                for g in split_by_palette(m['triangles'], vb):
                    s = self.jmesh(m, m.get('kind', 'skin'), g, uvmap_for(m), texs)
                    s.u74 = len(model.meshes)
                    model.meshes.append(s)
            if not model.meshes:
                raise BuildError('no skin / eye meshes')
            kids[key] = jboy.write(model)
        t = self.conv.t
        # (Mk as stock: DXT1, alpha 255 - a lower alpha shows red spots over the skin)
        mk.putalpha(255)
        tex_a = [('normal', W.dds_encode(normal, 'DXT5')), ('noise', self.conv.ttex['noise']), ('Mk', W.dds_encode(mk, 'DXT1'))]
        tex_a += [(n, d) for n, d in self.conv.ttex.items() if n.lower().startswith(('bloodhair', 'bloodhand', 'blood'))]
        kids[0xa] = W.tex_bundle_write(tex_a, t[0xa][:16])
        kids[0xc] = W.tex_bundle_write([('color', W.dds_encode(color, 'DXT5'))], t[0xc][:16])
        # damage masks Mask1-3 (0xd-0xf: A4R4G4B4 on the colour sheet's UVs; without them the skin shows red
        # spots): blank, in the template's own format
        for k in (0xd, 0xe, 0xf):
            if k in t:
                n, d = W.tex_bundle_read(t[k])[0]
                kids[k] = W.tex_bundle_write([(n, d[:128] + bytes(len(d) - 128))], t[k][:16])
        for k in (0xb, 0x28, 0x32, 0x50, 0x64, 0x7530, 0x7531, 0x7532):
            if k in t:
                kids[k] = t[k]
        for k in (0xc8, 0xc9):
            if k in t:
                kids[k] = self.portrait or t[k]
        # budget
        dropped = []
        pages, parts = model_budget.attire_pages(kids)
        for k in (0x7532, 0x7531, 0x7530):
            if pages <= model_budget.LIMIT:
                break
            if k in kids:
                del kids[k]
                dropped.append(hex(k))
                pages, parts = model_budget.attire_pages(kids)
        if pages > model_budget.LIMIT:
            raise BuildError('%s: %d vertex pages, the game has %d per wrestler (%s) - reduce the vertex / triangle count'
                             % (entry_name, pages, model_budget.LIMIT, ', '.join('%s %d' % kv for kv in parts.items())))
        if model_budget.parts_over(parts):
            raise BuildError('%s: a part over %d vertex pages (%s) - the match draws at most 2,048 per part'
                             % (entry_name, model_budget.PART_LIMIT,
                                ', '.join('%s %d' % kv for kv in model_budget.parts_over(parts).items())))
        nmesh = sum(len(jboy.read(kids[k]).meshes) for k in (0, 0x2710, 0x4e20) if k in kids)
        return kids, {'name': entry_name, 'meshes': nmesh, 'pages': pages, 'parts': parts, 'dropped': dropped}

    def build(self, out):
        import ch_tool
        _, groups = ch_tool.epk8_read(open(self.template, 'rb').read())
        names = [n for n, _ in groups[0][1] if n.strip(b'\0')]
        entries, rep = [], []
        for i, a in enumerate(self.spec['attires']):
            # (the template's id, the attire digit, kind 2: '00010212' - not the template's i-th entry name, which on
            # the entrance-gear superstars (103 / 139 / 145) can be a kind 4 entry, and past its own attires none)
            name = (names[0][:6] if names else b'000104') + b'%d2' % i
            nm = a.get('name')
            if nm:   # (only an entry name the game finds: this id, this attire digit, kind 2 - a display name is not one)
                if not (isinstance(nm, str) and nm.encode('latin-1', 'replace') == name):
                    self.warnings.append('attire %d: "name" %r is not the entry name %s the game looks up - ignored'
                                         % (i + 1, nm, name.decode()))
            if a.get('hair_chains'):   # (moving hair: the chains in this attire's node table, 0x3C / 0x3D written)
                keep = self.nodes, self.index
                self.nodes, self.index = self.with_bones(None, a['hair_chains'])
                try:
                    kids, r = self.attire(a, name.decode('latin-1'))
                finally:
                    self.nodes, self.index = keep
                kids.update(self.hair_kids())
                r['hair_chains'] = len(self.chain_roots)
            else:
                kids, r = self.attire(a, name.decode('latin-1'))
            entries.append((b'EMD ', name, W.pack_kids(kids)))
            rep.append(r)
            if a.get('entrance_gear'):   # (the same attire wearing its entrance gear: entry "<id><attire>4")
                gname = name[:7] + b'4'
                gk, gr = self.gear(a['entrance_gear'], gname.decode('latin-1'))
                entries.append((b'EMD ', gname, W.pack_kids(gk)))
                gr['kind'] = 4
                rep.append(gr)
        W.write_epk8(self.template, entries, out)
        return rep

    def with_bones(self, extra, chains=None):
        """the node table plus extra bones [{name, parent, position (world, 2011 space)}] appended (coat tails,
        hat): local translation from the parent's world frame, no rotation -> (nodes, index)"""
        c = self.conv
        nodes = [dict(n) for n in self.nodes]
        index = dict(self.index)
        Wn = []
        for j, nd in enumerate(nodes):
            fr = c.tframe[j]
            p = fr['parent'] if j else -1
            M = np.eye(4)
            M[:3, :3] = W.rot(fr['r'])
            M[:3, 3] = nd['t']
            Wn.append(M if p < 0 else Wn[p] @ M)
        for b in extra or []:
            if b['name'] in index:
                continue
            pi = index.get(b.get('parent'), index.get('mune', 0))
            P = Wn[pi]
            t = P[:3, :3].T @ (np.array(b['position'], float) - P[:3, 3])
            nodes.append({'name': b['name'][:15], 't': tuple(float(x) for x in t), 'r': (0.0, 0.0, 0.0), 'u40': 0,
                          'parent': pi, 'u48': (pi, 0, 0, 0), 'sphere': (0.0, 0.0, 0.0, 0.0)})
            M = np.eye(4)
            M[:3, 3] = t
            Wn.append(P @ M)
            index[b['name']] = len(nodes) - 1
        self.chain_roots = []
        for ci, joints in enumerate((chains or [])[:16]):   # (hair physics chains, docs/HAIR_PHYSICS_PLAN.md B1)
            hang = 'atama'
            if isinstance(joints, dict):   # (cloth: the chain's root parent - the solver reads it from the node table)
                hang = joints.get('parent') or 'atama'
                if hang not in index:
                    self.warnings.append('chain %d: no bone %r to hang from - on atama' % (ci, hang))
                    hang = 'atama'
                joints = joints.get('joints') or []
            P = [np.array(j, float) for j in joints]
            if len(P) < 4:   # (a chain needs a pivot, a pinned node, one that moves and a tip target)
                self.warnings.append('hair chain %d: %d joints, at least 4 needed - left out' % (ci, len(P)))
                continue
            root = 'kami_b%02d' % len(self.chain_roots)
            L = float(np.mean([np.linalg.norm(P[k + 1] - P[k]) for k in range(len(P) - 1)]))
            pi = index.get(hang, index.get('atama', 0))
            Rw = W.align_x(P[1] - P[0])
            Ra = Wn[pi][:3, :3]
            prev = None
            for k in range(len(P)):
                name = root if k == 0 else '%s_%02d' % (root, k - 1)
                if k == 0:
                    t, R = Ra.T @ (P[0] - Wn[pi][:3, 3]), Ra.T @ Rw
                else:
                    Rk = W.align_x(P[k + 1] - P[k]) if k + 1 < len(P) else Rw
                    t, R = np.array([L, 0.0, 0.0]), Rw.T @ Rk
                    Rw = Rk
                r = W.euler_zyx(R)
                par = pi if k == 0 else prev
                nodes.append({'name': name, 't': tuple(float(x) for x in t), 'r': (0.0, float(r[0]), float(r[1])),
                              'u40': struct.unpack('>I', struct.pack('>f', float(r[2])))[0], 'parent': 0,
                              'u48': (par, 0, 0, 0), 'sphere': (0.0, 0.0, 0.0, 0.0)})
                M = np.eye(4)
                M[:3, :3] = W.rot(r)
                M[:3, 3] = t
                Wn.append(Wn[par] @ M)
                prev = len(nodes) - 1
                index[name] = prev
            index['%s_%02d' % (root, len(P) - 2)] = index['%s_%02d' % (root, len(P) - 3)] if len(P) > 3 else index[root]
            self.chain_roots.append(root)   # (the tip is only a target: weights naming it go to the node before)
        return nodes, index

    def hair_kids(self):
        """0x3C (stock-like line per chain) and 0x3D (the template's collision, else ch102's) for the chains built"""
        if not getattr(self, 'chain_roots', None):
            return {}
        txt = '; hair chains (svr11_build): kami_bNN radius visc grav spring damp A lencorr bend movecorr\r\n' + ''.join(
            ' %s 0.1500 0.1000 0.0340 0.0000 0.0000 0 0.0000 0.0300 0.0000 \r\n' % r for r in self.chain_roots)
        col = self.conv.t.get(0x3d)
        if col is None:
            col = W.Conv(os.path.join(os.path.dirname(self.template), 'ch102.pac')).t[0x3d]
        return {0x3c: txt.encode('latin-1'), 0x3d: col}

    def gear(self, g, entry_name):
        """an entrance gear entry (kind 4): built like an attire on the node table plus its extra bones; no
        blood overlays, no portrait (the stock ones have neither)"""
        keep = self.nodes, self.index, getattr(self, '_faced', False)
        self.nodes, self.index = self.with_bones(g.get('extra_bones'), g.get('hair_chains'))
        self._faced = True   # (the face pivots were set from the attire)
        try:
            kids, r = self.attire(g, entry_name)
        finally:
            self.nodes, self.index, self._faced = keep
        for k in (0x7530, 0x7531, 0x7532, 0xc8, 0xc9):
            kids.pop(k, None)
        kids.update(self.hair_kids())
        return kids, r


def main(argv):
    if '--gear-from' in argv or '--gear-json' in argv:   # (entrance gear onto a pac: gear_port.py)
        import gear_port
        return gear_port.main(argv)
    ap = argparse.ArgumentParser()
    ap.add_argument('model')
    ap.add_argument('out')
    ap.add_argument('report_pos', nargs='?', metavar='report.json')
    ap.add_argument('--report')
    a = ap.parse_args(argv[1:])
    a.report = a.report or a.report_pos
    rep = {'ok': False, 'out': a.out, 'warnings': []}
    code = 0
    try:
        spec = json.load(open(a.model, encoding='utf-8'))
        b = Builder(spec, os.path.dirname(os.path.abspath(a.model)))
        rep['attires'] = b.build(a.out)
        rep['warnings'] = b.warnings
        try:   # (bone rests lined up with the stock ones: tools/prop_bones.py, B3; rewrites only when it fixes one)
            import prop_bones
            pr = prop_bones.fix_pac(a.out, out=a.out)
            if pr['fixed']:
                rep['prop_bones'] = prop_bones.report_line(pr)
        except Exception as e:   # noqa: BLE001 (never fails the build)
            rep['warnings'].append('bone rests not checked (prop_bones: %s)' % e)
        rep['ok'] = True
    except BuildError as e:
        rep['error'] = str(e)
        code = 3
    except (KeyError, ValueError, OSError) as e:
        rep['error'] = '%s: %s' % (type(e).__name__, e)
        code = 2
    s = json.dumps(rep, indent=1)
    if a.report:
        open(a.report, 'w', encoding='utf-8').write(s)
    print(s)
    return code


if __name__ == '__main__':
    sys.exit(main(sys.argv))
