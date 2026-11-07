"""SvR 2007 character model -> a ch.pac SvR 2011 can load (superstar mods).

    python svr07_char.py <2007 chNN.pac> <out ch.pac> --template <2011 ch104.pac> [--portrait <256 DDS>] [--id N]

2007 models are 2008's (EPAC/PACH/JBOY, the same skeleton minus kubi_x, read by
svr08_char.read08), but textured per body part: each mesh has its own diffuse
map and normal map (46 textures for Steve Austin: body, arm, head, pants,
boot, gear..., each with a _N normal map) and there is no AO map. SvR 2011
draws a model from one sheet set (color 2048 x 1024, normal 1024 x 1024, Mk
512 x 256 - stock sizes with full mips; other sizes rendered black and red in
the Sabu tests). So the part textures are baked into those sheets:

- each distinct diffuse map gets a box in the color sheet (shelf packing; all
  parts scaled by one factor until they fit, 4-pixel gaps), its normal map the
  same box of the normal sheet (a missing or dummy normal map: flat);
- every mesh's UVs are mapped into its box (u' = x0 + u * w);
- Mk is a flat AO (2007 has none): bruise mask 0, AO 255.
Then the model is built as svr08_char does (template node table, palettes by
name, the template's shader blocks, face animation, blood overlays,
portrait). Only the wrestler entries (EMD kind 2) are converted.
"""
import argparse
import io
import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import jboy  # noqa: E402
import svrfmt  # noqa: E402
import svr08_char as S8  # noqa: E402

COLOR = (2048, 1024)
NORMAL = (1024, 1024)
MK = (512, 256)
PAD = 4
SHINE = 48   # (the normal sheet's alpha: bake)
MISSING = '\0missing'   # (the box for meshes whose texture the pac doesn't carry)


def slot_names(m8, s8):
    """{param name: texture name (lower case)} of a 2007 mesh's texture slots"""
    import struct
    out = {}
    for name, typ, val in s8['params']:
        if typ == 0x0f:
            k = struct.unpack('>i', val[:4])[0]
            if 0 <= k < len(m8['textures']):
                out[name] = m8['textures'][k].lower()
    return out


def pack(sizes, W, H):
    """shelf packing of {name: (w, h)} into W x H at one scale -> (scale, {name: (x, y, w, h)})"""
    # first-fit decreasing height: each part goes on the first shelf with room for it
    order = sorted(sizes, key=lambda n: (-sizes[n][1], -sizes[n][0]))
    s = 1.0
    while s > 0.05:
        boxes, shelves, top = {}, [], 0   # shelves: [y, height, x used]
        ok = True
        for n in order:
            w, h = max(4, int(sizes[n][0] * s)), max(4, int(sizes[n][1] * s))
            sh = next((q for q in shelves if q[2] + w <= W and h <= q[1]), None)
            if sh is None:
                if w > W or top + h > H:
                    ok = False
                    break
                sh = [top, h, 0]
                shelves.append(sh)
                top += h + PAD
            boxes[n] = (sh[2], sh[0], w, h)
            sh[2] += w + PAD
        if ok:
            return s, boxes
        s *= 0.97
    raise RuntimeError('textures do not fit')


def bake(kids, legs=None):
    """-> ({child: m8 with sheet UVs}, color DDS, normal DDS, Mk DDS, notes)"""
    tex = {n.lower(): d for n, d in S8.tex_read(kids[0xa])}
    m8s = {k: S8.read08(kids[k]) for k in (0, 0x2710, 0x4e20) if k in kids and kids[k][:4] == b'JBOY'}
    for m8 in m8s.values():   # (a diva's straight legs onto the diva skeleton's: svr08_char.Template.leg_fit)
        S8.repose(m8, legs)
    diffuse_of, normal_of, missing = {}, {}, []
    for k, m8 in m8s.items():
        for i, s8 in enumerate(m8['meshes']):
            sl = slot_names(m8, s8)
            d = sl.get('texDiffuse')
            if d not in tex:
                # (a modded pac can name a texture it doesn't carry: left as it was, the mesh's UVs spanned the whole
                # baked sheet - every part's colours as specks; it gets a plain grey box instead, and a note)
                missing.append('%s mesh %d (%s)' % ({0: 'body', 0x2710: 'hair', 0x4e20: 'gear'}[k], i, d or 'no texture'))
                diffuse_of[(k, i)] = MISSING
                continue
            diffuse_of[(k, i)] = d
            n = sl.get('texNormal')
            if n in tex and not n.startswith('dummy'):
                normal_of.setdefault(d, n)
    images = {d: Image.open(io.BytesIO(tex[d])).convert('RGBA') for d in sorted(set(diffuse_of.values()) - {MISSING})}
    if missing:
        images[MISSING] = Image.new('RGBA', (8, 8), (128, 128, 128, 0))   # (sorted: a set's order follows the hash seed - the packed sheet changed every run)
    scale, boxes = pack({d: im.size for d, im in images.items()}, *COLOR)
    color = Image.new('RGBA', COLOR, (128, 128, 128, 255))
    # (the normal sheet's alpha is the skin shaders' shine mask - SpecLev x normal.a. 2007's skin normal maps keep a
    # mask (Kurt Angle's body 0-68), its gear / hands / knees ones are opaque, as was this sheet behind parts with no
    # normal map: those shone at full SpecLev, a white glare on SvR 2007 wrestlers - report 20261101-162315. 48, the
    # FBX import's "no normal map" shine, where there is no mask)
    normal = Image.new('RGBA', NORMAL, (128, 128, 255, SHINE))
    sx, sy = NORMAL[0] / COLOR[0], NORMAL[1] / COLOR[1]
    for d, (x, y, w, h) in boxes.items():
        # (S8.resize_rgba, not Image.resize: Pillow premultiplies RGBA, so where a 2007 part's alpha is near 0 -
        # Mick Foley's vest 0..32, pants 1..15 - its colour came out as dark streaks and coloured specks)
        color.paste(S8.resize_rgba(images[d], (w, h), Image.LANCZOS), (x, y))
        if d in normal_of:
            nb = (int(x * sx), int(y * sy), max(1, int(w * sx)), max(1, int(h * sy)))
            nim = S8.resize_rgba(Image.open(io.BytesIO(tex[normal_of[d]])).convert('RGBA'), nb[2:], Image.LANCZOS)
            if nim.getchannel('A').getextrema()[0] >= 250:   # (an opaque one: no mask of its own - 48)
                nim.putalpha(SHINE)
            normal.paste(nim, nb[:2])
    for (k, i), d in diffuse_of.items():
        x, y, w, h = boxes[d]
        s8 = m8s[k]['meshes'][i]
        s8['uvs'] = [((x + min(max(u, 0.0), 1.0) * w) / COLOR[0], (y + min(max(v, 0.0), 1.0) * h) / COLOR[1])
                     for u, v in s8['uvs']]
    ao = Image.new('L', MK, 255)
    mk = Image.merge('RGBA', (Image.new('L', MK, 0), ao, ao, Image.new('L', MK, 255)))
    notes = '%d part textures baked at %.0f%% (%s)' % (len(boxes), scale * 100, ', '.join(sorted(set(diffuse_of.values()) - {MISSING})))
    if missing:
        notes += '; NOT IN THE PAC, drawn grey: ' + ', '.join(missing)
    return m8s, S8.dds_encode(color, 'DXT5'), S8.dds_encode(normal, 'DXT5'), S8.dds_encode(mk, 'DXT5'), notes


def attire(kids, tp, portrait):
    """one 2011 attire (PACH children) from a 2007 EMD entry's children"""
    legs = tp.leg_fit(kids[0])
    m8s, color_d, normal_d, mk_d, notes = bake(kids, legs)
    if legs:
        notes += "; legs bent onto the template skeleton's"
    textures = ['color', 'normal', 'mk', 'noise', 'cubemap']
    SKIN = {'texDiffuse': 'color', 'texNormal': 'normal', 'texOcclusion': 'mk', 'texTilingMap': 'noise'}
    EYE = {'texDiffuse': 'color', 'texCubeRefction': 'cubemap', 'texRefctionReg': 'color', 'texOcclusion': 'mk'}
    nodes = tp.nodes_at(kids[0], legs)   # (the template's nodes on the source's joints: svr08_char.Template.nodes_at)
    out = {0: jboy.write(S8.model(tp, tp.body, [(m8s[0], lambda s: (tp.p_eye, EYE) if 'Reflect' in s['shader'] else (tp.p_skin, SKIN))],
                                  textures, nodes))}
    if 0x2710 in m8s:
        out[0x2710] = jboy.write(S8.model(tp, tp.hair, [(m8s[0x2710], lambda s: (tp.p_hair, SKIN))], textures[:5], nodes))
    if 0x4e20 in m8s:
        out[0x4e20] = jboy.write(S8.model(tp, tp.lash, [(m8s[0x4e20], lambda s: (tp.p_lash, {'texDiffuse': 'color'}))], ['color'], nodes))
    ta = [('normal', normal_d), ('noise', tp.ttex['noise']), ('Mk', mk_d)]
    ta += [(n, d) for n, d in tp.ttex.items() if n.lower().startswith('bloodhair')]
    out[0xa] = S8.tex_write(ta, tp.t[0xa][:16])
    out[0xb] = tp.t[0xb]
    out[0xc] = S8.tex_write([('color', color_d)], tp.t[0xc][:16])
    for k in (0x28, 0x32, 0x50, 0x64, 0x7530, 0x7531, 0x7532):
        if k in tp.t:
            out[k] = tp.t[k]
    for k in (0xc8, 0xc9):
        out[k] = portrait or tp.t[k]
    return out, notes


def convert(src, out, template, cid, portrait=None, limit=4):
    """-> [(2007 entry, 2011 entry, notes)], [entries left out]"""
    h, g, t = svrfmt.epac_read(open(src, 'rb').read())
    emd = next(es for ty, es in g if ty == b'EMD ')
    ents = []
    for n, b in emd:
        s = n.decode('latin1').strip('\0 ')
        if len(s) == 4 and s[3] == '2':
            ents.append((s, int(s[2]), {i: S8.unp(x) for i, x in svrfmt.pach_read(S8.unp(b))}))
    if not ents:
        raise RuntimeError('no wrestler (kind 2) model in %s' % os.path.basename(src))
    ents.sort(key=lambda e: e[1])
    tp, tcid = S8.template_for(template, ents[0][2][0])   # (a diva: 2011's diva skeleton, ch143)
    convert.skeleton = tcid
    entries, kept = [], []
    for k, (s, att, kids) in enumerate(ents[:limit]):
        o, notes = attire(kids, tp, portrait)
        name = b'%06d%02d' % (cid, k * 10 + 2)
        entries.append((name, svrfmt.pach_write([(i, svrfmt.bpe_encode(o[i], compress=True)) for i in sorted(o)])))
        kept.append((s, name.decode(), notes))
    S8.write_epk8(template, entries, out)
    return kept, [e[0] for e in ents[limit:]]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('out')
    ap.add_argument('--template', required=True)
    ap.add_argument('--portrait')
    ap.add_argument('--id', type=int, default=1)
    a = ap.parse_args()
    portrait = open(a.portrait, 'rb').read() if a.portrait else None
    kept, extra = convert(a.src, a.out, a.template, a.id, portrait)
    for s, n, notes in kept:
        print('attire', s, '->', n, notes)
    print('wrote', a.out, os.path.getsize(a.out), 'left out', extra)


if __name__ == '__main__':
    main()
