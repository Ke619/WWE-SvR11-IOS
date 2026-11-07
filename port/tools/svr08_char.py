"""SvR 2008 character model -> a ch.pac SvR2011 can load (superstar mods).

    python svr08_char.py <2008 chNN.pac> <out ch.pac> --template <2011 ch104.pac> [--portrait <256 DDS>] [--costume 0x20,...]

2008 models are EPAC/PACH/JBOY like 2011's, on the same skeleton in the same
units (2011's node names plus collision/helper bones k*). Differences: 2008
meshes use yBumpMap / yReflect / yDefault with older vertex formats, one
texture set (child 0xa: <id>color, <id>normal, <id>ao) and costume pieces as
extra JBOYs (0x20.., textures in 0x27). So: the 2011 template's node table,
palettes remapped by name (helpers to their nearest 2011 ancestor), the
template's parameter blocks (yBumpMapSS skin, yReflect eyes, hair, eyelash),
textures re-sorted into 2011's sets (0xa normal / noise / Mk, 0xc color),
Mk from the 2008 AO (bruise mask R = 0, AO in G / B), costume pieces merged
into the body, and the template's 0x28/0x32/0x50/0x64, cube map and blood
overlays.
"""
import argparse, io, os, struct, sys
import numpy as np
from PIL import Image

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
import ch_tool, svrfmt, jboy  # noqa: E402
from w13_char import tex_bundle_read as tex_read, resize_rgba, dds_encode  # noqa: E402 (the same texture helpers)

B = 8


unp = svrfmt.unbpe


def u32(d, o): return struct.unpack_from('>I', d, o)[0]
def cstr(b): return b.split(b'\0')[0].decode('latin-1')


def read08(d):
    """JBOY (mesh 0xB4) -> dict, true node layout (r +32, parent +48)"""
    nmesh, mptr, nnode, ntex, nptr, tptr, optr = struct.unpack_from('>7I', d, 0x18)
    m = {'name': cstr(d[B + optr:B + optr + 16]),
         'textures': [cstr(d[B + tptr + 16 * i:B + tptr + 16 * i + 16]) for i in range(ntex)], 'nodes': [], 'meshes': []}
    for i in range(nnode):
        o = B + nptr + 80 * i
        m['nodes'].append({'name': cstr(d[o:o + 16]), 'parent': struct.unpack_from('>i', d, o + 48)[0]})
    for i in range(nmesh):
        o = B + mptr + 0xB4 * i
        vc, nstrip, npal = struct.unpack_from('>3I', d, o)
        s = {'palette': list(struct.unpack_from('>20i', d, o + 0x0C))[:npal]}
        nw = max(1, u32(d, o + 0x5C))
        vblk, wptr, uvptr = struct.unpack_from('>3I', d, o + 0x68)
        s['shader'] = cstr(d[o + 0x78:o + 0x88])
        npar, parptr, iptr = struct.unpack_from('>3I', d, o + 0x90)
        vdat = B + u32(d, B + vblk)
        s['verts'] = [struct.unpack_from('>6fI', d, vdat + 28 * k) for k in range(vc)]
        s['nw'] = nw
        s['wraw'] = d[B + wptr:B + wptr + 8 * vc * nw]
        s['uvs'] = [struct.unpack_from('>2f', d, B + uvptr + 8 * k) for k in range(vc)]
        s['params'] = []
        for k in range(npar):
            po = B + u32(d, B + parptr + 4 * k)
            typ, size = struct.unpack_from('>HH', d, po + 16)
            s['params'].append((cstr(d[po:po + 16]), typ, d[po + 20:po + size]))
        s['strips'] = []
        for k in range(nstrip):
            prim, icount, idat = struct.unpack_from('>3I', d, B + iptr + 12 * k)
            s['strips'].append(list(struct.unpack_from('>%dH' % icount, d, B + idat)))
        m['meshes'].append(s)
    return m


def tex_write(texs, head):
    n = len(texs)
    off = (16 + 32 * n + 15) & ~15
    recs, blobs = b'', b''
    for name, dd in texs:
        recs += name.encode('latin-1')[:16].ljust(16, b'\0') + b'dds\0' + struct.pack('<II', len(dd), off + len(blobs)) + b'\0' * 4
        blobs += dd + b'\0' * (-len(dd) % 16)
    return (struct.pack('<I', n) + head[4:16] + recs).ljust(off, b'\0') + blobs


def mk_from_ao(ao_dds):
    im = Image.open(io.BytesIO(ao_dds)).convert('RGB')
    g = im.convert('L')
    return dds_encode(Image.merge('RGBA', (Image.new('L', im.size, 0), g, g, Image.new('L', im.size, 255))), 'DXT5')


class Template:
    def __init__(self, path):
        _, groups = ch_tool.epk8_read(open(path, 'rb').read())
        self.t = {i: ch_tool.unpack(x) for i, x in svrfmt.pach_read(ch_tool.unpack(groups[0][1][0][1]))}
        self.body = jboy.read(self.t[0])
        self.hair = jboy.read(self.t[0x2710])
        self.lash = jboy.read(self.t[0x4e20])
        d = self.t[0]
        nnode, nptr = u32(d, 0x20), u32(d, 0x28)
        self.parent = [struct.unpack_from('>i', d, B + nptr + 80 * i + 48)[0] for i in range(nnode)]
        self.index = {n['name']: i for i, n in enumerate(self.body.nodes)}
        ms = self.body.meshes
        from w13_char import skin_template
        self.p_skin = skin_template(ms)   # (not the first skin mesh: see skin_template)
        self.p_eye = next(s for s in ms if s.shader == 'yReflect')
        self.p_hair = self.hair.meshes[-1]
        self.p_lash = self.lash.meshes[0]
        self.ttex = dict(tex_read(self.t[0xa]))

    def leg_fit(self, body_jboy):
        """{source node name: 4 x 4} that bends the source body's legs onto the template's, or {} when they already
        match. 2007-2009 divas stand on 2011's diva skeleton (ch143) but with straight legs, ch143's 15 degrees
        apart: with each node only moved onto the source joint (nodes_at), every leg bone's offset pointed 15
        degrees (the foot 56) off its frame's +X - the game's IK turns the template's frames, so the knees and
        ankles came out bent and twisted (report 30 Oct, Lita 2007). Each leg bone gets the turn that lays its
        source segment on the template's, about the template's joint; nodes under a leg bone follow it."""
        from w13_char import read13_nodes, world
        src, tf = read13_nodes(body_jboy), read13_nodes(self.t[0])
        Ws, Wt = world(src), world(tf)
        S = {nd['name']: Ws[i][:3, 3] for i, nd in enumerate(src)}
        T = {nd['name']: Wt[i][:3, 3] for i, nd in enumerate(tf)}
        fit, worst = {}, 0.0
        for side in ('l_', 'r_'):
            chain = [side + n for n in ('momo', 'sune', 'ashi', 'tsumasaki')]
            if any(n not in S or n not in T for n in chain):
                continue
            rot = None
            for a, b in zip(chain, chain[1:] + [None]):
                if b is not None:
                    vs, vt = S[b] - S[a], T[b] - T[a]
                    if np.linalg.norm(vs) < 1e-6 or np.linalg.norm(vt) < 1e-6:
                        return {}
                    rot = _turn(vs, vt)
                    worst = max(worst, float(np.degrees(np.arccos(np.clip(np.dot(vs, vt) / np.linalg.norm(vs) / np.linalg.norm(vt), -1, 1)))))
                M = np.eye(4)
                M[:3, :3] = rot   # (the toe keeps the foot's turn)
                M[:3, 3] = T[a] - rot @ S[a]
                fit[a] = M
        if worst < 3.0:   # (a body already on its template's legs: left as it is)
            return {}
        for i, nd in enumerate(src):   # (twist helpers, toes: their nearest leg bone's)
            k = i
            while k >= 0 and src[k]['name'] not in fit:
                k = src[k]['parent']
            if k >= 0 and nd['name'] not in fit:
                fit[nd['name']] = fit[src[k]['name']]
        return fit

    def nodes_at(self, body_jboy, fit=None):
        """the template's node table with each node on the source body's joint of the same name (the template's
        frames; a node the source lacks keeps its offset from its parent) - as w13_char.nodes_for. fit (leg_fit):
        those joints moved by it first (the legs then sit on the template's own)."""
        from w13_char import read13_nodes, world
        src = read13_nodes(body_jboy)
        Ws = world(src)
        want = {}
        for i, nd in enumerate(src):
            p = Ws[i][:3, 3]
            if fit and nd['name'] in fit:
                p = fit[nd['name']][:3, :3] @ p + fit[nd['name']][:3, 3]
            want.setdefault(nd['name'], p)
        tf = read13_nodes(self.t[0])
        Wt = world(tf)
        nodes = [dict(nd) for nd in self.body.nodes]
        pos = []
        for j, nd in enumerate(nodes):
            pj = tf[j]['parent']
            if pj >= 0 and nd['name'] in want:
                nd['t'] = tuple(float(x) for x in Wt[pj][:3, :3].T @ (want[nd['name']] - pos[pj]))
                pos.append(np.array(want[nd['name']], dtype=float))
            elif pj >= 0:
                pos.append(pos[pj] + Wt[pj][:3, :3] @ np.array(tf[j]['t']))
            else:
                pos.append(Wt[j][:3, 3].copy())
        return nodes


def _turn(a, b):
    """the rotation taking direction a onto direction b (the shortest)"""
    a, b = a / np.linalg.norm(a), b / np.linalg.norm(b)
    v, c = np.cross(a, b), float(np.dot(a, b))
    if np.linalg.norm(v) < 1e-9:
        return np.eye(3) if c > 0 else -np.eye(3)
    K = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + K + K @ K * (1.0 / (1.0 + c))


def repose(m8, fit):
    """m8's vertices and normals moved by leg_fit's turns, each vertex by its weights' blend -> m8"""
    if not fit:
        return m8
    names = [n['name'] for n in m8['nodes']]
    for s8 in m8['meshes']:
        vc, nw, w, pal = len(s8['verts']), s8['nw'], s8['wraw'], s8['palette']
        out = []
        for k, v in enumerate(s8['verts']):
            P, N, tot = np.zeros(3), np.zeros(3), 0.0
            p, n = np.array(v[:3]), np.array(v[3:6])
            for j in range(nw):   # (vertex-major: a palette index byte, then the weight)
                o = 8 * (nw * k + j)
                pi, wt = w[o], struct.unpack_from('>f', w, o + 4)[0]
                if pi == 255 or wt <= 0 or pi >= len(pal) or pal[pi] <= 0:
                    continue
                M = fit.get(names[pal[pi] - 1])
                P += wt * (p if M is None else M[:3, :3] @ p + M[:3, 3])
                N += wt * (n if M is None else M[:3, :3] @ n)
                tot += wt
            if tot <= 0:
                out.append(v)
                continue
            P /= tot
            N = N / (np.linalg.norm(N) or 1.0)
            out.append((float(P[0]), float(P[1]), float(P[2]), float(N[0]), float(N[1]), float(N[2])) + tuple(v[6:]))
        s8['verts'] = out
    return m8


# 2011's own skeletons a 2007-2010 body can be bound to: the men's (ch104, the converters' template) and the divas'
# (Mickie James ch143: arms 40 degrees down, upper arm 1.7, the hands at the hips - 2007 / 2008 / 2009 divas use this
# very skeleton; Melina ch174: arms out). Every node of a model sits where its template puts it, so a diva's mesh on
# ch104's bones hung each hand vertex off a joint a hand's width away: in the game the hands looked wrong and the
# face moved oddly, in the Mod Maker 3D view the fingers stretched out like spider legs (report 22 Oct, Lita 2007).
SKELETON_TEMPLATES = (104, 143, 174)
_SKEL_JOINTS = ('l_ninoude', 'r_ninoude', 'l_kote', 'r_kote', 'l_te', 'r_te', 'l_yubi21', 'r_yubi21',
                'l_momo', 'r_momo', 'l_sune', 'r_sune', 'l_ashi', 'r_ashi', 'kubi', 'atama')
_templates = {}


def skeleton_gap(jboy_a, jboy_b):
    """the mean distance (2011 units, 10 cm) between two JBOY bodies' joints of the same names (_SKEL_JOINTS)"""
    from w13_char import read13_nodes, world
    pos = []
    for d in (jboy_a, jboy_b):
        nodes = read13_nodes(d)
        W = world(nodes)
        pos.append({nd['name']: W[i][:3, 3] for i, nd in enumerate(nodes)})
    common = [n for n in _SKEL_JOINTS if n in pos[0] and n in pos[1]]
    if not common:
        return float('inf')
    return float(np.mean([np.linalg.norm(pos[0][n] - pos[1][n]) for n in common]))


def template_for(path, body_jboy):
    """the Template whose skeleton the source body (its kind-2 JBOY) is bound to: `path` (ch104) or, when the
    source's joints sit nearer another of SKELETON_TEMPLATES in the same folder (a diva's), that one. ->
    (Template, its ch id or None for path)"""
    folder = os.path.dirname(path)
    best = None
    for cid in (None,) + tuple(c for c in SKELETON_TEMPLATES if c != 104):
        q = path if cid is None else os.path.join(folder, 'ch%d.pac' % cid)
        if not os.path.exists(q):
            continue
        if q not in _templates:
            try:
                _templates[q] = Template(q)
            except Exception:   # noqa: BLE001  (a modded or odd stock pac: not a candidate)
                _templates[q] = None
        tp = _templates[q]
        if tp is None:
            continue
        gap = skeleton_gap(body_jboy, tp.t[0])
        if best is None or gap < best[0] - 0.05:   # (ch104 unless another is clearly nearer)
            best = (gap, tp, cid)
    if best is None:
        return Template(path), None
    return best[1], best[2]


# 2008 sway chains with no 2011 bone, pinned to a steadier one than their parent: the turban's
# cloth tails (7004 kami_a06 back, a07 / a08 rear right / left - both parented to r_sakotsu in 2008)
PIN = {'kami_a06': 'mune', 'kami_a07': 'mune', 'kami_a08': 'mune'}


def node_map(m8, tp):
    out = {}
    for i, nd in enumerate(m8['nodes']):
        pin = next((v for p, v in PIN.items() if nd['name'].startswith(p)), None)
        if pin in tp.index:
            out[i] = tp.index[pin]
            continue
        k = i
        while k >= 0 and m8['nodes'][k]['name'] not in tp.index:
            k = m8['nodes'][k]['parent']
        out[i] = tp.index[m8['nodes'][k]['name']] if k >= 0 else 0
    return out


def mesh(s8, tmpl, nmap, textures, slots):
    s = jboy.Mesh()
    s.raw = bytes(tmpl.raw)
    s.u04 = 1
    s.u74 = 0
    s.shader = tmpl.shader
    s.vfmt = tmpl.vfmt
    s.palette = [nmap[p - 1] + 1 if p > 0 else p for p in s8['palette']]
    s.verts = [(v[0], v[1], v[2], v[3], v[4], v[5], 0xFFFFFFFF) for v in s8['verts']]
    vc, nw, w = len(s.verts), s8['nw'], s8['wraw']
    s.weights = [[(tuple(w[8 * vc * j + 8 * k:8 * vc * j + 8 * k + 4]), struct.unpack_from('>f', w, 8 * vc * j + 8 * k + 4)[0])
                  for k in range(vc)] for j in range(nw)]
    s.uvs = list(s8['uvs'])
    s.strips = [(6, idx) for idx in s8['strips']]
    s.params = []
    for name, typ, val in tmpl.params:
        if typ == 0x0f and struct.unpack('>i', val[:4])[0] >= 0:   # (a slot the template leaves unbound stays so)
            want = slots.get(name)
            k = textures.index(want) if want in textures else -1
            val = struct.pack('>i', k) + val[4:]
        s.params.append((name, typ, val))
    P = np.array([v[:3] for v in s.verts])
    c = (P.min(0) + P.max(0)) / 2
    r = float(np.sqrt(((P - c) ** 2).sum(1)).max())
    md = bytearray(s.raw)
    struct.pack_into('>I', md, 0x8C, tmpl.vfmt)
    struct.pack_into('>4f', md, 0xA4, float(c[0]), float(c[1]), float(c[2]), r)
    s.raw = bytes(md)
    return s


def model(tp, tmodel, parts, textures, nodes=None):
    """parts: [(m8, kind(s8) -> (template mesh, slots))]; nodes: the node table (Template.nodes_at), else the
    template's"""
    m = jboy.Model()
    m.header, m.name, m.group = tmodel.header, tmodel.name, tmodel.group
    m.name_tail = getattr(tmodel, 'name_tail', b'')
    m.textures = textures
    m.nodes = nodes if nodes is not None else tp.body.nodes
    m.meshes = []
    for m8, kind in parts:
        nmap = node_map(m8, tp)
        for s8 in m8['meshes']:
            tmpl, slots = kind(s8)
            ms = mesh(s8, tmpl, nmap, textures, slots)
            ms.u74 = len(m.meshes)
            m.meshes.append(ms)
    return m


def dds_img(d):
    return Image.open(io.BytesIO(d)).convert('RGBA')


def remap_uvs(m8, su, sv, ou, ov):
    for s8 in m8['meshes']:
        s8['uvs'] = [(ou + u * su, ov + v * sv) for u, v in s8['uvs']]
    return m8


def attire(kids, tp, portrait, costume_ids, first_kids):
    """One 2011 attire. Costume pieces (2008 children 0x20.., own textures in 0x27) go in the
    body model on the body's own textures, as 2011 models are made: the costume's colour,
    normal and AO are painted into the body sheets' free box (the hair strip, u 0.8125-1,
    v 0-0.625: an attire with costume pieces has no hair model) and its UVs mapped there.
    Every texture keeps the stock size and full mips. Tested in game (2026-10-08): extra
    texture names (cos_c...), a 2048 sheet and a mip-less sheet all rendered the attire
    black and red; the body mesh with attire 1's sheets rendered fine."""
    base = dict(tex_read(kids[0xa]))
    base0 = dict(tex_read(first_kids[0xa]))   # (attire 2 shares attire 1's color / normal)
    # (by the name's end, else anywhere in it: Carlito's 2008 sheet is "1802color_01")
    both = list(base.items()) + list(base0.items())
    pick = lambda suffix: (next((d for n, d in both if n.lower().endswith(suffix)), None) or
                           next((d for n, d in both if suffix in n.lower() and 'blood' not in n.lower()), None))
    color, normal = pick('color'), pick('normal') or pick('nomal')   # (2009 Maria's sheet is "12702nomal")
    ao = next((d for n, d in base.items() if n.lower().endswith('ao')), None) or pick('ao')
    cos = dict(tex_read(kids[0x27])) if 0x27 in kids else {}
    # (the costume sheets' names: "_c" / "_n" / "ao" mostly; 2008 Rey Mysterio's are 2304_color / 2304_nomal / 2304_AO)
    cpick = lambda *suffixes: next((d for s in suffixes for n, d in cos.items() if n.lower().endswith(s)), None)
    pieces = [cid for cid in costume_ids if cid in kids and kids[cid][:4] == b'JBOY'] if cos else []
    textures = ['color', 'normal', 'mk', 'noise', 'cubemap']
    SKIN = {'texDiffuse': 'color', 'texNormal': 'normal', 'texOcclusion': 'mk', 'texTilingMap': 'noise'}
    EYE = {'texDiffuse': 'color', 'texCubeRefction': 'cubemap', 'texRefctionReg': 'color', 'texOcclusion': 'mk'}
    atlas = bool(pieces)
    BOX = (0.8175, 0.0, 0.995, 0.62)   # (u0, v0, u1, v1) inside the free strip, small margins
    legs = tp.leg_fit(kids[0])   # (a diva's straight legs onto the diva skeleton's: Template.leg_fit)
    parts = [(repose(read08(kids[0]), legs), lambda s: (tp.p_eye, EYE) if 'Reflect' in s['shader'] else (tp.p_skin, SKIN))]
    for cid in pieces:
        parts.append((remap_uvs(repose(read08(kids[cid]), legs), BOX[2] - BOX[0], BOX[3] - BOX[1], BOX[0], BOX[1]), lambda s: (tp.p_skin, SKIN)))
    fit = lambda m8: m8
    nodes = tp.nodes_at(kids[0], legs)
    out = {0: jboy.write(model(tp, tp.body, parts, textures, nodes))}
    if 0x2710 in kids:
        out[0x2710] = jboy.write(model(tp, tp.hair, [(fit(read08(kids[0x2710])), lambda s: (tp.p_hair, SKIN))], textures[:5], nodes))
    if 0x4e20 in kids:
        out[0x4e20] = jboy.write(model(tp, tp.lash, [(fit(read08(kids[0x4e20])), lambda s: (tp.p_lash, {'texDiffuse': 'color'}))], ['color'], nodes))
    if atlas:
        def paint(body, piece):
            w, h = body.size
            x0, y0, x1, y1 = int(BOX[0] * w), int(BOX[1] * h), int(round(BOX[2] * w)), int(round(BOX[3] * h))
            im = body.copy()
            if piece is not None:   # (a costume sheet that isn't there: the box keeps the body sheet's pixels)
                im.paste(resize_rgba(piece, (x1 - x0, y1 - y0), Image.LANCZOS), (x0, y0))
            return im
        piece = lambda d, mode='RGBA': dds_img(d).convert(mode) if d else None
        color_d = dds_encode(paint(dds_img(color), piece(cpick('_c', 'color'))), 'DXT5')
        normal_d = dds_encode(paint(dds_img(normal), piece(cpick('_n', 'normal', 'nomal'))), 'DXT5')
        ao_img = paint(dds_img(ao).convert('L'), piece(cpick('ao'), 'L'))
        mk_d = dds_encode(Image.merge('RGBA', (Image.new('L', ao_img.size, 0), ao_img, ao_img, Image.new('L', ao_img.size, 255))), 'DXT5')
    else:
        color_d, normal_d, mk_d = color, normal, mk_from_ao(ao)
    ta = [('normal', normal_d), ('noise', tp.ttex['noise']), ('Mk', mk_d)]
    ta += [(n, d) for n, d in tp.ttex.items() if n.lower().startswith('bloodhair')]
    out[0xa] = tex_write(ta, tp.t[0xa][:16])
    out[0xb] = tp.t[0xb]
    out[0xc] = tex_write([('color', color_d)], tp.t[0xc][:16])
    for k in (0x28, 0x32, 0x50, 0x64, 0x7530, 0x7531, 0x7532):
        if k in tp.t:
            out[k] = tp.t[k]
    for k in (0xc8, 0xc9):
        out[k] = portrait or tp.t[k]
    return out


def write_epk8(tmpl, entries, out):
    t = open(tmpl, 'rb').read()
    header = bytearray(t[:0x800])
    index = bytearray(0x3800)
    struct.pack_into('<4sHH', index, 0, b'EMD ', 4 * len(entries), 0)
    index[6:12] = t[0x806:0x80C]
    body = bytearray()
    p = 12
    for name, blob in entries:
        while len(body) % 0x800:
            body.append(0)
        index[p:p + 8] = name.ljust(8, b'\0')[:8]
        struct.pack_into('<II', index, p + 8, len(body) // 0x800, (len(blob) + 0xFF) // 0x100)
        body += blob
        p += 16
    while len(body) % 0x800:
        body.append(0)
    struct.pack_into('<I', header, 4, 12 + 16 * len(entries))
    struct.pack_into('<I', header, 8, len(body))
    open(out, 'wb').write(bytes(header) + bytes(index) + bytes(body) + bytes(0x800))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('out')
    ap.add_argument('--template', required=True)
    ap.add_argument('--portrait')
    ap.add_argument('--costume', default='0x20')
    ap.add_argument('--id', type=int, default=70)
    a = ap.parse_args()
    tp = Template(a.template)
    portrait = open(a.portrait, 'rb').read() if a.portrait else None
    cos = [int(x, 0) for x in a.costume.split(',') if x]
    h, g, t = svrfmt.epac_read(open(a.src, 'rb').read())
    ents = [(n, {i: unp(x) for i, x in svrfmt.pach_read(unp(b))}) for n, b in g[0][1]]
    first = ents[0][1]
    entries = []
    for k, (n, kids) in enumerate(ents):
        out = attire(kids, tp, portrait, cos, first)
        name = b'%06d%02d' % (a.id, k * 10 + 2)
        entries.append((name, svrfmt.pach_write([(i, svrfmt.bpe_encode(out[i], compress=True)) for i in sorted(out)])))
        print('attire', n.decode(), '->', name.decode(), sorted(hex(i) for i in out), flush=True)
    write_epk8(a.template, entries, a.out)
    print('wrote', a.out, os.path.getsize(a.out))


if __name__ == '__main__':
    main()
