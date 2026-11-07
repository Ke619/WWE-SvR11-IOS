"""Create A Superstar parts from an OBJ and a PNG: a mod folder for Mods/CawParts (docs/CAW_PARTS.md, src/caw_parts.cpp).

  python caw_part.py make --game-pac <pac dir> --obj <file> --png <file> --out <mod folder> --id <id> [--name <text>]
                     [--category 18] [--template 5] [--genders mf] [--thumb <png>] [--no-fit] [--made-with <text>]
  python caw_part.py export --game-pac <pac dir> --out <folder> [--category 18] [--template 5]
        -> the template part as template.obj + template.png, to model around

A part is built on a stock part of the category (--template, a record of its MENU list): its shaders, colour slots, settings
and normal map. The OBJ's first object replaces the template's first mesh, a second object its second (glasses: the lenses,
drawn as glass); with none, that mesh is one tiny triangle no one sees (the game walks a model's meshes assuming the
template's count). The part is rigid, on the head bone. By default the
OBJ, as Blender writes one (y up, the front towards +z), is turned, scaled to the template's width and put on its front;
--no-fit keeps its numbers (the game's: y down, the front towards -z, as `export` writes them).

The PNG is the part's colour texture, made the template's size and 256 colours. The player's colour choices multiply it
(both start white). Nothing of the game goes in the mod but what a part must have: read from the player's own disc at make
time, like the other porters; a mod made here is for the maker's own game.

Exit 0 done, 2 bad input, 3 failed. stdout: one line of JSON.
"""
import io
import json
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jboy  # noqa: E402
import svrfmt  # noqa: E402
from svr11_build import stripify  # noqa: E402

REC = 60
# (categories played: glasses. Piercings (44) list but don't draw yet; hair and hats have rigs and chains)
SIMPLE = {18}


def unpack(b):
    return svrfmt.bpe_decode(b) if b[:4] == b'BPE ' else b


def pack(b):
    """a part's entries are read through the game's BPE decoder: packed like the stock ones"""
    return svrfmt.bpe_encode(b, compress=True)


def menu_record(pac, category, record):
    _, groups, _ = svrfmt.epac_read(open(os.path.join(pac, 'edit', 'edit.pac'), 'rb').read())
    top = dict(groups[0][1])
    cats = dict(svrfmt.pach_read(unpack(dict(svrfmt.pach_read(unpack(top[b'MENU'])))[0])))
    if category not in cats:
        raise ValueError('no category %d' % category)
    k = unpack(cats[category])
    total = struct.unpack_from('<H', k, 0)[0]
    if not 0 <= record < total:
        raise ValueError('category %d has %d records' % (category, total))
    return bytes(k[12 + record * REC:12 + (record + 1) * REC])


def part_entries(pac, category, rec):
    model, tex = struct.unpack_from('<HH', rec, 12)
    if model == 0xffff:
        raise ValueError('that record has no model')
    _, groups, _ = svrfmt.epac_read(open(os.path.join(pac, 'edit', 'ed%02d.pac' % category), 'rb').read())
    e = dict(groups[0][1])
    return dict(svrfmt.pach_read(unpack(e[b'M%03X' % model]))), dict(svrfmt.pach_read(unpack(e[b'T%03X' % tex])))


def thumb_of(pac, category, record):
    _, groups, _ = svrfmt.epac_read(open(os.path.join(pac, 'edit', 'casThumb.pac'), 'rb').read())
    return unpack(dict(svrfmt.pach_read(unpack(dict(groups[0][1])[b'%04X' % category])))[record])


def read_obj(path):
    """-> (positions, uvs, normals, [triangles of (p, t, n)] per object, the first two)"""
    pos, uv, nor, objs = [], [], [], [[]]
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            w = line.split()
            if not w:
                continue
            if w[0] in ('o', 'g'):
                if objs[-1]:
                    objs.append([])
            elif w[0] == 'v':
                pos.append(tuple(float(x) for x in w[1:4]))
            elif w[0] == 'vt':
                uv.append((float(w[1]), float(w[2])))
            elif w[0] == 'vn':
                nor.append(tuple(float(x) for x in w[1:4]))
            elif w[0] == 'f':
                c = []
                for t in w[1:]:
                    q = t.split('/')
                    idx = [int(q[0]), int(q[1]) if len(q) > 1 and q[1] else 0, int(q[2]) if len(q) > 2 and q[2] else 0]
                    c.append((idx[0] - 1 if idx[0] > 0 else len(pos) + idx[0],
                              idx[1] - 1 if idx[1] > 0 else (len(uv) + idx[1] if idx[1] else -1),
                              idx[2] - 1 if idx[2] > 0 else (len(nor) + idx[2] if idx[2] else -1)))
                for i in range(1, len(c) - 1):
                    objs[-1].append((c[0], c[i], c[i + 1]))
    objs = [o for o in objs if o]
    if not objs:
        raise ValueError('no faces in the OBJ')
    return pos, uv, nor, objs[:2]


def box(points):
    return [min(p[a] for p in points) for a in range(3)], [max(p[a] for p in points) for a in range(3)]


def fit(points, target):
    """an OBJ as Blender writes it (y up, its front towards +z) -> the game's (y down, front towards -z): turned 180 degrees
    about x, scaled to the template's width, centred on it across and up, its front on the template's front"""
    turned = [(p[0], -p[1], -p[2]) for p in points]
    lo, hi = box(turned)
    tlo, thi = box(target)
    s = (thi[0] - tlo[0]) / ((hi[0] - lo[0]) or 1.0)
    cx, cy = (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2
    tx, ty = (tlo[0] + thi[0]) / 2, (tlo[1] + thi[1]) / 2
    return lambda p: ((p[0] - cx) * s + tx, (-p[1] - cy) * s + ty, (-p[2] - lo[2]) * s + tlo[2])


def flat_normal(a, b, c):
    u = [b[i] - a[i] for i in range(3)]
    v = [c[i] - a[i] for i in range(3)]
    n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
    d = math.sqrt(sum(x * x for x in n)) or 1.0
    return tuple(x / d for x in n)


def build_mesh(mesh, verts, uvs, tris, weights):
    mesh.verts = verts
    mesh.uvs = uvs
    mesh.weights = weights
    mesh.vc2 = len(verts)
    mesh.strips = [(mesh.strips[0][0], stripify(tris))]
    lo, hi = box([v[:3] for v in verts])
    c = tuple((lo[a] + hi[a]) / 2 for a in range(3))
    mesh.sphere = c + (math.sqrt(max(sum((v[a] - c[a]) ** 2 for a in range(3)) for v in verts)),)


def rebuild(model_bytes, parts, bone):
    """a template JBOY with its meshes' shapes replaced: parts = [(points, normals, uvs, triangles)], one per mesh. Each
    vertex is on one bone at full weight, the head's: the in-game model's own blocks aren't plain weights
    (they don't add up to 1), so they can't be moved to another vertex (it flew off)."""
    m = jboy.read(model_bytes)
    if len(m.meshes) != len(parts):
        raise ValueError('the template has %d meshes' % len(m.meshes))
    for t, (pts, nrs, uvs, tris) in zip(m.meshes, parts):
        colour = t.verts[0][6]
        verts = [tuple(p) + tuple(n) + (colour,) for p, n in zip(pts, nrs)]
        t.palette = [bone]
        build_mesh(t, verts, uvs, tris, [[((0, 0, 0, 0), 1.0)] * len(pts)])
    return jboy.write(m)


def main_bone(model_bytes):
    """the in-game template's bone most vertices use (the two models' bones are numbered apart: the editor's isn't in a match)"""
    m = jboy.read(model_bytes).meshes[0]
    n = {}
    for blk in m.weights:
        for bones, _ in blk:
            if bones[0] != 255:
                n[bones[0]] = n.get(bones[0], 0) + 1
    return m.palette[max(n, key=n.get)]


def vertex_list(blob, counts):
    """the model's third entry: for each editor vertex, the in-game model's (mesh, 0, vertex) that gives its place once
    skinned. Here both models have one vertex order."""
    head = bytearray(blob[:16])
    struct.pack_into('>I', head, 4, sum(counts))
    out = bytes(head)
    for mesh, count in enumerate(counts):
        for i in range(count):
            out += struct.pack('>3I', mesh, 0, i) + b'\xcc\xcc\xcc\xcc'
    return out


def colour_texture(png, tga):
    """the PNG as the template TGA's size: 18-byte header, 256 x BGRA palette, 8-bit rows (bottom first, as the game's)"""
    from PIL import Image
    w, h = struct.unpack_from('<HH', tga, 12)
    idl, cl = tga[0], struct.unpack_from('<H', tga, 5)[0]
    im = Image.open(png).convert('RGBA').resize((w, h), Image.LANCZOS)
    alpha = im.getchannel('A')
    q = im.convert('RGB').quantize(255, method=Image.MEDIANCUT)
    pal = q.getpalette()[:765]
    data = bytearray(q.tobytes())
    # (the top of the PNG first, or last: from the template's descriptor)
    if not tga[17] & 0x20:
        data = bytearray(b''.join(bytes(data[y * w:(y + 1) * w]) for y in range(h - 1, -1, -1)))
    out = bytearray(tga[:18 + idl])
    table = bytearray()
    a = alpha.resize((1, 1)).getpixel((0, 0))
    has_alpha = alpha.getextrema()[0] < 255
    for i in range(cl):
        r, g, b = pal[i * 3:i * 3 + 3] if i * 3 + 2 < len(pal) else (0, 0, 0)
        table += bytes((b, g, r, 255))
    if has_alpha:  # (the index 255 is see-through)
        table[255 * 4:255 * 4 + 4] = bytes((0, 0, 0, 0))
        src = alpha.tobytes()
        if not tga[17] & 0x20:
            src = b''.join(src[y * w:(y + 1) * w] for y in range(h - 1, -1, -1))
        for i, v in enumerate(src):
            if v < 128:
                data[i] = 255
    return bytes(out + table + data)


def write_mod(out, mid, name, category, genders, rec, model, tex, thumb, made=''):
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, 'part.rec'), 'wb').write(rec)
    open(os.path.join(out, 'part.m.pach'), 'wb').write(model)
    open(os.path.join(out, 'part.t.pach'), 'wb').write(tex)
    open(os.path.join(out, 'part.thumb.dds'), 'wb').write(thumb)
    with open(os.path.join(out, 'manifest.txt'), 'w', newline='\r\n', encoding='utf-8') as f:
        f.write('type=cawpart\nid=%s\nname=%s\nversion=1.0\n%spart=%d %s part\n' % (mid, name, 'made_with=%s\n' % made if made else '', category, genders))


def dxt1(im):
    """an RGB PIL image (each side a multiple of 4) -> DXT1 blocks: each block's darkest and brightest as its ends"""
    w, h = im.size
    px = im.load()
    out = bytearray()

    def c565(c):
        return (c[0] >> 3) << 11 | (c[1] >> 2) << 5 | c[2] >> 3
    for y in range(0, h, 4):
        for x in range(0, w, 4):
            b = [px[x + i, y + j] for j in range(4) for i in range(4)]
            hi, lo = max(b, key=sum), min(b, key=sum)
            c0, c1 = c565(hi), c565(lo)
            if c0 < c1:
                c0, c1, hi, lo = c1, c0, lo, hi
            ends = [hi, lo, tuple((2 * p + q) // 3 for p, q in zip(hi, lo)), tuple((p + 2 * q) // 3 for p, q in zip(hi, lo))]
            bits = 0
            for k, c in enumerate(b):
                d = [sum((u - v) ** 2 for u, v in zip(c, e)) for e in ends]
                bits |= d.index(min(d)) << (2 * k)
            out += struct.pack('<HHI', c0, c1, bits if c0 != c1 else 0)
    return bytes(out)


def make_thumb(path):
    """256 x 128 DXT1 with one mip level, as casThumb's"""
    from PIL import Image
    im = Image.open(path).convert('RGB')
    w, h = im.size
    s = min(256 / w, 128 / h)
    im = im.resize((max(1, int(w * s)), max(1, int(h * s))), Image.LANCZOS)
    c = Image.new('RGB', (256, 128), (40, 40, 40))
    c.paste(im, ((256 - im.size[0]) // 2, (128 - im.size[1]) // 2))
    head = struct.pack('<4sIIIIIII44xII4s20xI16x', b'DDS ', 124, 0x81007, 128, 256, 16384, 0, 1, 32, 4, b'DXT1', 0x1000)
    return head + dxt1(c)


def cmd_make(a):
    cat, tpl = int(a.get('--category', 18)), int(a.get('--template', 5))
    if cat not in SIMPLE:
        raise ValueError('category %d: only %s so far' % (cat, sorted(SIMPLE)))
    pac = a['--game-pac']
    rec = menu_record(pac, cat, tpl)
    if rec[20] == 0:
        raise ValueError('template %d has no colour slots: pick another (a part with none does not draw in this category)' % tpl)
    mdl, tx = part_entries(pac, cat, rec)
    keys = sorted(mdl)
    if len(keys) != 3:
        raise ValueError('the template has %d model entries, not 3' % len(keys))
    edit_key, game_key, list_key = keys
    edit_b, game_b = unpack(mdl[edit_key]), unpack(mdl[game_key])
    meshes = jboy.read(edit_b).meshes
    if len(meshes) > 2:
        raise ValueError('the template has %d meshes, 2 at most so far' % len(meshes))
    pos, uv, nor, objs = read_obj(a['--obj'])
    tp = [v[:3] for v in meshes[0].verts]
    used = [pos[c[0]] for tri in objs[0] for c in tri]
    place = (lambda p: tuple(p)) if '--no-fit' in a else fit(used, tp)
    parts = []
    for o in range(len(meshes)):
        if o < len(objs):
            index, pts, nrs, uvs, out = {}, [], [], [], []
            for tri in objs[o]:
                fn = flat_normal(*[pos[c[0]] for c in tri])
                face = []
                for c in tri:
                    key = c if c[2] >= 0 else (c[0], c[1], tuple(fn))
                    if key not in index:
                        index[key] = len(pts)
                        pts.append(place(pos[c[0]]))
                        n0 = nor[c[2]] if c[2] >= 0 else fn
                        nrs.append(tuple(n0) if '--no-fit' in a else (n0[0], -n0[1], -n0[2]))
                        u = uv[c[1]] if c[1] >= 0 else (0.0, 0.0)
                        uvs.append((u[0], 1.0 - u[1]))
                    face.append(index[key])
                out.append(tuple(face))
        else:  # (a triangle no one sees, inside the part)
            c = tp[0]
            pts = [c, (c[0] + 1e-4, c[1], c[2]), (c[0], c[1] + 1e-4, c[2])]
            nrs = [(0.0, 0.0, -1.0)] * 3
            uvs = [(0.0, 0.0)] * 3
            out = [(0, 1, 2)]
        parts.append((pts, nrs, uvs, out))
    counts = [len(q[0]) for q in parts]
    if sum(counts) > 65000:
        raise ValueError('%d vertices: too many' % sum(counts))
    model = svrfmt.pach_write([(edit_key, pack(rebuild(edit_b, parts, meshes[0].palette[0]))),
                               (game_key, pack(rebuild(game_b, parts, main_bone(game_b)))),
                               (list_key, pack(vertex_list(unpack(mdl[list_key]), counts)))])
    tk = sorted(tx)
    tex = svrfmt.pach_write([(tk[0], pack(colour_texture(a['--png'], unpack(tx[tk[0]]))))] + [(k, tx[k]) for k in tk[1:]])
    write_mod(a['--out'], a['--id'], a.get('--name', a['--id']), cat, a.get('--genders', 'mf'), rec, model, tex,
              make_thumb(a.get('--thumb', a['--png'])), a.get('--made-with', ''))
    return dict(ok=True, out=a['--out'], vertices=sum(counts), triangles=sum(len(q[3]) for q in parts), category=cat, template=tpl)


def cmd_export(a):
    from PIL import Image
    cat, tpl = int(a.get('--category', 18)), int(a.get('--template', 5))
    pac = a['--game-pac']
    rec = menu_record(pac, cat, tpl)
    mdl, tx = part_entries(pac, cat, rec)
    os.makedirs(a['--out'], exist_ok=True)
    m = jboy.read(unpack(mdl[sorted(mdl)[0]])).meshes[0]
    with open(os.path.join(a['--out'], 'template.obj'), 'w', newline='\n') as f:
        f.write('o template\n')
        for v in m.verts:
            f.write('v %.6f %.6f %.6f\n' % v[:3])
        for u, v in m.uvs:
            f.write('vt %.6f %.6f\n' % (u, 1.0 - v))
        for v in m.verts:
            f.write('vn %.6f %.6f %.6f\n' % v[3:6])
        for a0, b0, c0 in jboy.strip_to_tris(m.strips[0][1]):
            f.write('f %d/%d/%d %d/%d/%d %d/%d/%d\n' % ((a0 + 1,) * 3 + (b0 + 1,) * 3 + (c0 + 1,) * 3))
    t = unpack(tx[sorted(tx)[0]])
    w, h = struct.unpack_from('<HH', t, 12)
    idl, cl = t[0], struct.unpack_from('<H', t, 5)[0]
    pal = t[18 + idl:18 + idl + cl * 4]
    rows = t[18 + idl + cl * 4:18 + idl + cl * 4 + w * h]
    im = Image.new('RGBA', (w, h))
    px = bytearray()
    for i in rows:
        px += bytes((pal[i * 4 + 2], pal[i * 4 + 1], pal[i * 4], pal[i * 4 + 3]))
    im.frombytes(bytes(px))
    if not t[17] & 0x20:
        im = im.transpose(Image.FLIP_TOP_BOTTOM)
    im.save(os.path.join(a['--out'], 'template.png'))
    return dict(ok=True, out=a['--out'], vertices=len(m.verts))


def main(argv):
    if len(argv) < 2 or argv[1] not in ('make', 'export'):
        print(__doc__, file=sys.stderr)
        return 2
    a, i = {}, 2
    while i < len(argv):
        if argv[i] == '--no-fit':
            a[argv[i]] = True
            i += 1
        elif i + 1 < len(argv):
            a[argv[i]] = argv[i + 1]
            i += 2
        else:
            print(json.dumps(dict(ok=False, error='%s needs a value' % argv[i])))
            return 2
    need = ('--game-pac', '--obj', '--png', '--out', '--id') if argv[1] == 'make' else ('--game-pac', '--out')
    miss = [k for k in need if k not in a]
    if miss:
        print(json.dumps(dict(ok=False, error='missing ' + ' '.join(miss))))
        return 2
    try:
        print(json.dumps(cmd_make(a) if argv[1] == 'make' else cmd_export(a)))
    except (ValueError, OSError, KeyError, struct.error) as x:
        print(json.dumps(dict(ok=False, error=str(x))))
        return 3 if isinstance(x, (struct.error, KeyError)) else 2
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
