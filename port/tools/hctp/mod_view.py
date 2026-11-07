"""Render a built backstage mod (arena.pac parking models + gimmick.pac T23 objects at their locators,
placed the way 2011 shows them: local (x, z) -> (x cos a - z sin a, x sin a + z cos a) + pos)."""
import base64, io, math, os, struct, sys
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # (its own folder: the embeddable Python doesn't add it)
import svr08_stage as s8, svr08_gimmick as G, svrfmt as f, jboy, arena_tool as at
import to_glb

class M: pass

def dds_tex(bundle):
    tex = {}
    for n, dds in at.tex_bundle(bundle):
        try:
            tex[n.rsplit('.', 1)[0]] = np.array(Image.open(io.BytesIO(dds)).convert('RGBA'))
        except Exception:
            pass
    return tex

def from_jboy(j, xform=None):
    m = M(); m.name = j.name; m.textures = j.textures; m.meshes = []
    for s in j.meshes:
        slot = [struct.unpack('>i', v[:4])[0] for n, t, v in s.params if n == 'texDiffuse']
        slot = slot[0] if slot else -1
        P = np.array([v[:3] for v in s.verts], np.float64); N = np.array([v[3:6] for v in s.verts], np.float64)
        if xform:
            P = xform(P); N = xform(N, True)
        strips = []
        for prim, idx in s.strips:
            st = []
            for k in idx:
                c = s.verts[k][6]
                st.append((k, (s.uvs[k][0], s.uvs[k][1], 1.0), (((c >> 16) & 255) / 510, ((c >> 8) & 255) / 510, (c & 255) / 510, (c >> 24) / 510)))
            strips.append(st)
        m.meshes.append(dict(pos=P, nrm=N, subs=[dict(tex=slot, strips=strips)]))
    return m

def main(moddir, out):
    _, _, _, _, e = s8.stage(moddir + '/arena.pac')
    d = {i: s8.unpack(b) for i, b in e}
    tex = dds_tex(d[400])
    models = []
    for i in range(20):
        if d.get(i, b'')[:4] == b'JBOY':
            models.append((f'stage{i:02d}', from_jboy(jboy.read(d[i])), None))
    entry = next((l.strip()[8:] for l in open(moddir + '/manifest.txt') if l.startswith('gimmick=')), '78H3')
    (h, g, t), _, pk = G.find(moddir + '/gimmick.pac', b'GMGB', entry.encode())
    sub = {i: G.unpack(b) for i, b in f.pach_read(dict(f.pach_read(pk))[0])}
    W = struct.unpack(f'<{len(sub[2]) // 4}I', sub[2])
    objs = {}
    for i, b in f.pach_read(sub[4]):
        u = G.unpack(b)
        if i == 0:
            tex.update(dds_tex(u))
        elif u[:4] == b'JBOY':
            objs[i] = jboy.read(u)
    recs = {rid: (typ, W[off:off + n]) for rid, typ, n, off in G.records(sub[0])}
    L = G.unpack(dict(f.pach_read(sub[3]))[10000])
    n = struct.unpack_from('<I', L, 0)[0]
    debug = []
    for k in range(n):
        x, y, z, idx, rx, ry, rz, _ = struct.unpack_from('<3fI3fI', L, 16 + 32 * k)
        rid = idx & 0xFFFF if idx < 65536 else idx >> 17
        typ, words = recs.get(idx, recs.get(rid, (None, None)))
        if typ == 23:
            mid = int(G.fval(words[0]))
            if mid in objs:
                c, s_ = math.cos(ry), math.sin(ry)
                def xf(P, normal=False, c=c, s_=s_, x=x, y=y, z=z):
                    Q = np.empty_like(P)
                    Q[:, 0] = P[:, 0] * c - P[:, 2] * s_
                    Q[:, 2] = P[:, 0] * s_ + P[:, 2] * c
                    Q[:, 1] = P[:, 1]
                    if not normal:
                        Q += (x, y, z)
                    return Q
                models.append((f'obj{mid}_r{idx}', from_jboy(objs[mid], xf), None))
        elif typ in (21, 22):
            from room_view import box
            debug.append((f'T{typ} r{idx} key {int(G.fval(words[2]))} ry {ry:.2f}', box(x, y - 3, z, 1.5, 3, 1.5), (0.1, 0.4, 1, 0.9)))
            debug.append((f'T{typ} r{idx} facing', box(x + 4 * math.sin(ry), y - 3, z + 4 * math.cos(ry), 0.7, 0.7, 0.7), (1, 1, 0, 0.9)))
    R = s8.hmd_read(d[1004])
    debug.append(('collision 1004', [np.array(s8.tri_corners(r)) for r in R if abs(r[12]) < 1e5], (1, 0.1, 0.1, 0.3)))
    glb = to_glb.build(models, tex, out + '.glb', debug)
    open(out + '.html', 'w').write(to_glb.VIEWER.replace('%GLB%', base64.b64encode(glb).decode()).replace('%TITLE%', 'mod ' + out))
    print(out, len(models), 'models', len(debug), 'markers')

main(sys.argv[1], sys.argv[2])
