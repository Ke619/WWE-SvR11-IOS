"""WWE '13 character model -> a ch.pac SvR2011 can load (superstar mods).

    python w13_char.py <WWE13 chNNN.pac (decoded EPK8)> <out ch.pac> --template <2011 chNNN.pac> [--portrait <256 x 256 DDS>]

WWE '13 models (EPK8 / EMD / PACH / JBOY, BPE) use a 3ds Max Biped skeleton
named "Bip ..." in cm with the floor at 0; SvR2011 uses its own names in 10 cm
units with the pelvis at 0. The rigs are the same (joint positions and rest
pose agree at 0.1 scale), so bones are renamed and re-parented by name, the
vertices scaled, palettes remapped. JBOY meshes are 0xB8 bytes (an extra u32
at +0x74); shaders yCh_* become SvR2011's yBumpMapSS / yReflect / yDefault
with a 2011 model's parameter blocks. The 2011 template gives the skeleton's
node frames and the children WWE '13 lacks (face animation 0x64, 0x50, 0x28,
cube map, tiling noise).
"""
import argparse, io, math, os, struct, sys
import numpy as np
from PIL import Image

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
import ch_tool, svrfmt, jboy  # noqa: E402

B = 8

# ---------------------------------------------------------------- reading
def u32(d, o): return struct.unpack_from('>I', d, o)[0]
def cstr(b): return b.split(b'\0')[0].decode('latin-1')


def read13(d):
    """WWE '13 JBOY -> dict (meshes 0xB8)."""
    nmesh, mptr, nnode, ntex, nptr, tptr, optr = struct.unpack_from('>7I', d, 0x18)
    m = {'name': cstr(d[B + optr:B + optr + 16]),
         'textures': [cstr(d[B + tptr + 16 * i:B + tptr + 16 * i + 16]) for i in range(ntex)],
         'nodes': [], 'meshes': []}
    for i in range(nnode):
        o = B + nptr + 80 * i
        m['nodes'].append({'name': cstr(d[o:o + 16]), 't': struct.unpack_from('>3f', d, o + 16),
                           'r': struct.unpack_from('>3f', d, o + 32), 'parent': struct.unpack_from('>i', d, o + 48)[0]})
    for i in range(nmesh):
        o = B + mptr + 0xB8 * i
        vc, nstrip, npal = struct.unpack_from('>3I', d, o)
        s = {'palette': list(struct.unpack_from('>20i', d, o + 0x0C))[:npal]}
        nw = u32(d, o + 0x5C)
        vblk, wptr, uvptr = struct.unpack_from('>3I', d, o + 0x68)
        s['shader'] = cstr(d[o + 0x7C:o + 0x8C])
        s['vfmt'] = u32(d, o + 0x90)
        npar, parptr, iptr = struct.unpack_from('>3I', d, o + 0x94)
        vdat = B + u32(d, B + vblk)
        s['verts'] = [struct.unpack_from('>6fI', d, vdat + 28 * k) for k in range(vc)]
        s['nw'] = max(1, nw)
        s['wraw'] = d[B + wptr:B + wptr + 8 * vc * s['nw']]
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


def read13_nodes(d):
    nnode, nptr = u32(d, 0x20), u32(d, 0x28)
    out = []
    for i in range(nnode):
        o = B + nptr + 80 * i
        out.append({'name': cstr(d[o:o + 16]), 't': struct.unpack_from('>3f', d, o + 16),
                    'r': struct.unpack_from('>3f', d, o + 32), 'parent': struct.unpack_from('>i', d, o + 48)[0]})
    return out


def tex_bundle_read(b):
    n = struct.unpack_from('<I', b, 0)[0]
    out = []
    for i in range(n):
        r = b[16 + 32 * i:48 + 32 * i]
        sz, off = struct.unpack_from('<II', r, 20)
        out.append((r[:16].split(b'\0')[0].decode('latin-1'), b[off:off + sz]))
    return out


def tex_bundle_write(texs, head=None):
    n = len(texs)
    off = (16 + 32 * n + 15) & ~15
    recs, blobs = b'', b''
    for name, dd in texs:
        recs += name.encode('latin-1')[:16].ljust(16, b'\0') + b'dds\0' + struct.pack('<II', len(dd), off + len(blobs)) + b'\0' * 4
        blobs += dd + b'\0' * (-len(dd) % 16)
    hdr = struct.pack('<I', n) + (head[4:16] if head else b'\0' * 12)
    return (hdr + recs).ljust(off, b'\0') + blobs


# ---------------------------------------------------------------- skeleton
def rot(r):
    x, y, z = r
    cx, sx, cy, sy, cz, sz = math.cos(x), math.sin(x), math.cos(y), math.sin(y), math.cos(z), math.sin(z)
    Rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    Rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    return Rz @ Ry @ Rx


def world(nodes, tkey='t', rkey='r'):
    W = []
    for n in nodes:
        M = np.eye(4)
        M[:3, :3] = rot(n[rkey])
        M[:3, 3] = n[tkey]
        W.append(M if n['parent'] < 0 else W[n['parent']] @ M)
    return W


def name13to11(n):
    """WWE '13 bone -> SvR2011 node (None: keep looking up the parents)."""
    side = None
    for pre, s in (('Bip L ', 'l'), ('Bip R ', 'r'), ('L_', 'l'), ('R_', 'r'), ('l_', 'l'), ('r_', 'r')):
        if n.startswith(pre):
            side, rest = s, n[len(pre):]
            break
    fixed = {'Bip Pelvis': 'root', 'Bip': 'root', 'null': 'root', 'vector': 'root',
             'Bip Spine': 'koshi', 'Bip Spine1': 'mune', 'Bip Neck': 'kubi', 'Neck_x': 'kubi_x', 'Bip Head': 'atama',
             'd_kuchi_t': 'd_kuchi'}
    if n in fixed:
        return fixed[n]
    if n.startswith('kami_b15'):  # (the tie's physics chain, neck to waist: chest, then waist)
        return 'mune' if n in ('kami_b15', 'kami_b15_00', 'kami_b15_01', 'kami_b15_02') else 'koshi'
    if n.startswith('kami_'):  # (hair physics chains: with the head)
        return 'atama'
    if side:
        limb = {'Thigh': 'momo', 'Thigh_u': 'momo', 'Thigh_x': 'momo_x', 'Calf': 'sune', 'Foot': 'ashi', 'Toe0': 'tsumasaki',
                'Clavicle': 'sakotsu', 'Chest_Dummy': 'mune_dummy', 'Clavicle_m': 'sakotsu_m', 'Shoulder_m': 'kata_m',
                'UpperArm': 'ninoude', 'UpperArm_x': 'ninoude_x', 'UpperArm_m': 'ninoude_m', 'Elbow_j': 'kote',
                'Forearm': 'kote', 'Elbow_m': 'kote_m', 'Wrist_x': 'kote_x', 'Wrist_x2': 'kote_x2', 'Hand': 'te'}
        if rest in limb:
            return side + '_' + limb[rest]
        if rest.startswith('Finger'):
            f = rest[6:]
            return '%s_yubi%s%s' % (side, f[0], f[1:] if len(f) > 1 else '0')
    return n  # face bones share names


def cloth_root(nodes, i):
    """A physics chain that doesn't hang from the head - cloth, as Rikishi 2K17's tassels on the thighs: the bone it
    hangs from, so its vertices stay with that. name13to11 takes every kami_ for hair and the head's: skinned between
    his hips and his head, the tassels stretched into long strands. Any other bone: itself. (The tie, kami_b15: as
    name13to11 has it.)"""
    if not nodes[i]['name'].startswith('kami_') or nodes[i]['name'].startswith('kami_b15'):
        return i
    k = i
    while k >= 0 and nodes[k]['name'].startswith('kami_'):
        k = nodes[k]['parent']
    return k if k >= 0 and nodes[k]['name'] != 'Bip Head' else i


# bones whose WWE '13 position sets the 2011 node's (one per node)
PRIMARY = {'Bip Pelvis', 'Bip Spine', 'Bip Spine1', 'Bip Neck', 'Neck_x', 'Bip Head', 'd_kuchi_t'}


def is_primary(n13, n11):
    if n13 in PRIMARY:
        return True
    if n13.startswith(('kami_', 'vector', 'null')) or n13 in ('Bip',):
        return False
    if n13.endswith(('Thigh_u', 'Elbow_j')):
        return False
    return True


# ---------------------------------------------------------------- conversion
def skin_template(meshes):
    """The body skin mesh to copy parameters from: the parameter set most yBumpMapSS meshes share
    (ch104: 22 of 27 - tiling noise 100 x 50, tiling effect 0.02 / 0.15 / 0.1), without a wrinkle,
    muscle or bruise part, the largest of those. (The first skin mesh is a special one: tile count 1,
    tiling effect 1.0 - with the noise bound as its tiling map it paints red spots over the body.)
    -> a copy with the wrinkle / muscle / bruise part numbers 0."""
    import collections
    skin = [s for s in meshes if s.shader == 'yBumpMapSS']

    def sig(s):
        return tuple((n, v) for n, t, v in s.params if t != 0x0f and not n.startswith(('g_f2Wrinkle', 'g_f2Muscle', 'g_i')))

    def parts(s):
        return sum(struct.unpack('>i', v[:4])[0] != 0 for n, t, v in s.params
                   if n in ('g_iWrinkleParts', 'g_iMuscleParts', 'g_iBruiseParts'))
    common = collections.Counter(sig(s) for s in skin).most_common(1)[0][0]
    pick = [s for s in skin if sig(s) == common]
    best = min(pick, key=lambda s: (parts(s), -len(s.verts)))
    # (a part number picks a region of the wrinkle / muscle maps the converted models don't carry: none)
    out = jboy.Mesh()
    out.__dict__.update(best.__dict__)
    zero = struct.pack('>i', 0)
    out.params = [(n, t, zero + v[4:] if n in ('g_iWrinkleParts', 'g_iMuscleParts', 'g_iBruiseParts') else v)
                  for n, t, v in best.params]
    return out


# ---------------------------------------------------------------- hair physics chains (docs/HAIR_PHYSICS.md)

def _align_x(d):
    """the rotation turning +X to direction d (shortest arc)"""
    d = np.asarray(d, float)
    d = d / (np.linalg.norm(d) or 1.0)
    x = np.array([1.0, 0.0, 0.0])
    v = np.cross(x, d)
    c = float(np.dot(x, d))
    if np.linalg.norm(v) < 1e-9:
        return np.eye(3) if c > 0 else np.diag([-1.0, -1.0, 1.0])
    k = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + k + k @ k * (1.0 / (1.0 + c))


def _euler_zyx(R):
    """R = Rz Ry Rx (rot()) -> (x, y, z)"""
    y = math.asin(max(-1.0, min(1.0, -R[2, 0])))
    if abs(R[2, 0]) < 0.99999:
        return (math.atan2(R[2, 1], R[2, 2]), y, math.atan2(R[1, 0], R[0, 0]))
    return (0.0, y, math.atan2(-R[0, 1], R[1, 1]))


align_x, euler_zyx = _align_x, _euler_zyx   # (svr11_build builds its chains with them)


def hair_chains(m13):
    """WWE '13 hair model -> {root name: [node indices, root first]}: kami_bNN, kami_bNN_00, _01 ... (the tie's
    kami_b15 stays folded into the chest / waist)"""
    names = {nd['name']: i for i, nd in enumerate(m13['nodes'])}
    out = {}
    for nn in range(16):
        root = 'kami_b%02d' % nn
        if nn == 15 or root not in names:
            continue
        chain = [names[root]]
        k = 0
        while '%s_%02d' % (root, k) in names:
            chain.append(names['%s_%02d' % (root, k)])
            k += 1
        if len(chain) >= 3 and cloth_root(m13['nodes'], chain[0]) == chain[0]:
            out[root] = chain
    return out


HAIR_LINES_MAX = 16   # (the most chain lines a stock 2011 0x3C has)
# The most an attire's models (every JBOY child, unpacked) may take when its hair moves (chain lines in 0x3C): the
# largest stock attire (ch117, 9 chains). A wrestler's models share a fixed area with what the moving hair
# needs: Asuka 2K17 (6 chains, 1 067 988 bytes) ran out building her blood overlay (blood_face: the game crashed
# copying into a null allocation, sub_826B6BA8 -> sub_8215A000) or hung on NOW LOADING; without the blood
# (963 864) or with still hair (1 027 412) she plays, and still-haired ports load at up to 1 086 232 (Rollins 2K17).
# Since 2.1.6 the game gives a full model heap fallback memory where Asuka's blood overlay ran out (model_mem.cpp: the
# copy at sub_826B6BA8): a 38,000-vertex test model (2 MB blood overlay copy) plays in 1v1 / 6-man / Royal Rumble.
CHAIN_MODEL_BYTES_MAX = 4000000


def hair_chain_count(text):
    """the chains a 0x3C names (lines: name + 9 numbers)"""
    return len({ln.split()[0] for ln in text.replace(b'\r', b'').split(b'\n')
                if len(ln.split()) >= 10 and not ln.split()[0].startswith(b';')})


def model_bytes(kids):
    """an attire's models (JBOY children, unpacked) in bytes"""
    return sum(len(v) for v in kids.values() if v[:4] == b'JBOY')


def hair_text_11(text13, roots):
    """WWE '13 0x3C (SpringHairSetting: kami_bNN radius visc grav spring damp A lencorr bend movecorr, in cm) ->
    2011's lines for these chains: the same columns, radius and gravity in 2011 units (x 0.1). 2K15+ files
    ("Version2") hold several Setting blocks naming the same chains: each chain once (its first line), at most
    HAIR_LINES_MAX - 2011's own lists never name a chain twice or pass 16, and one that did (Asuka 2K17: 6 chains
    x 4 blocks = 24 lines) crashed or hung the match load"""
    lines = [b'; hair chains from WWE \'13 (w13_char): kami_bNN radius visc grav spring damp A lencorr bend movecorr']
    seen = set()
    for raw in text13.replace(b'\r', b'').split(b'\n'):
        tok = raw.split()
        if not tok or tok[0].startswith(b';') or tok[0].decode('latin-1') not in roots or len(tok) < 10:
            continue
        if tok[0] in seen or len(seen) >= HAIR_LINES_MAX:
            continue
        try:
            v = [float(x) for x in tok[1:10]]
        except ValueError:
            continue
        seen.add(tok[0])
        v[0] *= 0.1
        v[2] *= 0.1
        lines.append((' %s %.4f %.4f %.4f %.4f %.4f %d %.4f %.4f %.4f ' % (
            tok[0].decode('latin-1'), v[0], v[1], v[2], v[3], v[4], int(v[5]), v[6], v[7], v[8])).encode('latin-1'))
    return b'\r\n'.join(lines) + b'\r\n'


def hair_text_problems(text):
    """a 2011 0x3C against what the game takes -> [problems]: a chain named twice, more than HAIR_LINES_MAX
    chains (the Mod Maker's CheckHairSettings says the same; a chain the models haven't is fine - 23 stock
    attires name some)"""
    named = {}
    for raw in text.replace(b'\r', b'').split(b'\n'):
        tok = raw.split()
        if len(tok) < 10 or tok[0].startswith(b';'):
            continue
        named[tok[0]] = named.get(tok[0], 0) + 1
    out = ['%s named %d times' % (k.decode('latin-1'), v) for k, v in named.items() if v > 1]
    if sum(named.values()) > HAIR_LINES_MAX:
        out.append('%d chains, the game takes %d' % (sum(named.values()), HAIR_LINES_MAX))
    return out


class Conv:
    def __init__(self, tmpl_pac, portrait=None):
        self.tmpl_pac = tmpl_pac
        self.portrait = open(portrait, 'rb').read() if portrait else None
        _, groups = ch_tool.epk8_read(open(tmpl_pac, 'rb').read())
        n, b = groups[0][1][0]
        self.t = {i: ch_tool.unpack(x) for i, x in svrfmt.pach_read(ch_tool.unpack(b))}
        self.body = jboy.read(self.t[0])
        # (some templates have no hair model, ch139: hair then takes the body's skin block)
        self.hair = jboy.read(self.t[0x2710]) if 0x2710 in self.t else None
        self.lash = jboy.read(self.t[0x4e20])
        self.tnodes = self.body.nodes           # jboy.py dicts (written back as read)
        self.tframe = read13_nodes(self.t[0])    # true frames (r at +32, parent at +48)
        self.tindex = {nd['name']: i for i, nd in enumerate(self.tnodes)}
        # parameter blocks by kind
        ms = self.body.meshes
        self.p_skin = skin_template(ms)
        self.p_eye = next(s for s in ms if s.shader == 'yReflect')
        self.p_hair = self.hair.meshes[-1] if self.hair else self.p_skin
        self.p_lash = self.lash.meshes[0]
        self.ttex = dict(tex_bundle_read(self.t[0xa]))

    def world11(self, nodes):
        """world 4x4 of a 2011 node table built from the template (true frames: tframe r, the table's t); appended
        nodes carry their own true r ('r13') and parent ('p13')"""
        W = []
        for j, nd in enumerate(nodes):
            if j < len(self.tframe):
                r, p = self.tframe[j]['r'], self.tframe[j]['parent']
            else:
                r, p = nd['r13'], nd['p13']
            M = np.eye(4)
            M[:3, :3] = rot(r)
            M[:3, 3] = nd['t']
            W.append(M if p < 0 else W[p] @ M)
        return W

    def chain_nodes(self, nodes, m13, origin, chains):
        """the hair model's 2011 table: nodes + each chain (docs/HAIR_PHYSICS.md B1): kami_bNN at the strand's root
        under atama, every next node (L, 0, 0) - one L a chain, the mean of the WWE '13 segments - turned so +X
        points at the following one -> (table, {13 node index: 2011 index})"""
        W13 = world(m13['nodes'])
        out = [dict(nd) for nd in nodes]
        Wn = self.world11(out)
        ia = self.tindex.get('atama', 0)
        proto = dict(nodes[ia])
        idx = {}

        def add(name, t, R, parent):
            r = _euler_zyx(R)
            nd = dict(proto, name=name, t=tuple(float(x) for x in t), r=(0.0, float(r[0]), float(r[1])),
                      u40=struct.unpack('>I', struct.pack('>f', float(r[2])))[0], parent=0, u48=(parent, 0, 0, 0),
                      sphere=(0.0, 0.0, 0.0, 0.0), r13=r, p13=parent)
            out.append(nd)
            M = np.eye(4)
            M[:3, :3] = rot(r)
            M[:3, 3] = nd['t']
            Wn.append(Wn[parent] @ M)
            return len(out) - 1

        for root, chain in chains.items():
            P = [0.1 * (W13[i][:3, 3] - origin) for i in chain]
            L = float(np.mean([np.linalg.norm(P[k + 1] - P[k]) for k in range(len(P) - 1)]))
            Rw = _align_x(P[1] - P[0])
            Ra = Wn[ia][:3, :3]
            j = add(m13['nodes'][chain[0]]['name'], Ra.T @ (P[0] - Wn[ia][:3, 3]), Ra.T @ Rw, ia)
            idx[chain[0]] = j
            for k in range(1, len(chain)):
                Rk = _align_x(P[k + 1] - P[k]) if k + 1 < len(chain) else Rw
                j = add(m13['nodes'][chain[k]]['name'], (L, 0.0, 0.0), Rw.T @ Rk, j)
                idx[chain[k]] = j
                Rw = Rk
            idx[chain[-1]] = idx[chain[-2]]   # (the tip is only a target: its weights go to the node before)
        for nd in out:
            nd.pop('r13', None)
            nd.pop('p13', None)
        return out, idx

    def nodes_for(self, m13):
        """2011 node table with the WWE '13 model's proportions, and the 13 -> 11 node map."""
        n13 = m13['nodes']
        W13 = world(n13)
        pelvis = next(i for i, nd in enumerate(n13) if nd['name'] == 'Bip Pelvis')
        origin = W13[pelvis][:3, 3]
        idx13 = {nd['name']: i for i, nd in enumerate(n13)}
        # target world positions for 2011 nodes
        want = {}
        for i, nd in enumerate(n13):
            n11 = name13to11(nd['name'])
            if n11 in self.tindex and is_primary(nd['name'], n11) and n11 not in want:
                want[n11] = 0.1 * (W13[i][:3, 3] - origin)
        nodes = [dict(nd) for nd in self.tnodes]
        Wn = []
        for j, nd in enumerate(nodes):
            fr = self.tframe[j]
            p = fr['parent']
            if nd['name'] in want and p >= 0:
                Pp = Wn[p][:3, 3]
                Rp = Wn[p][:3, :3]
                nd['t'] = tuple(float(x) for x in Rp.T @ (want[nd['name']] - Pp))
            M = np.eye(4)
            M[:3, :3] = rot(fr['r'])
            child = LIMB_AIM.get(nd['name'])
            if child in want and nd['name'] in want and p >= 0:
                # (the limb bone's x along the source's limb: WWE '12 binds the legs straight down and the forearms
                # 15 degrees lower than 2011's A-pose - on the template's frames the knees / elbows sat off the bone's
                # axis and the game bent arms and legs the wrong way, report 22 Oct. '13 / 2K14 bind as 2011 does:
                # nothing turns. 2011's own Mickie James has other arm rests than ch104's and plays.)
                d = np.asarray(want[child]) - np.asarray(want[nd['name']])
                if np.linalg.norm(d) > 1e-6:
                    Rw = Wn[p][:3, :3] @ M[:3, :3]
                    Rw = _align(Rw[:, 0], d / np.linalg.norm(d)) @ Rw
                    M[:3, :3] = Wn[p][:3, :3].T @ Rw
                    set_true_r(nd, _euler_zyx(M[:3, :3]))
            M[:3, 3] = nd['t']
            Wn.append(M if p < 0 else Wn[p] @ M)
        # 13 -> 11 index map (walking up the parents when a bone has no 2011 node)
        nmap = {}
        for i, nd in enumerate(n13):
            k = cloth_root(n13, i)
            while True:
                n11 = name13to11(n13[k]['name'])
                if n11 in self.tindex:
                    nmap[i] = self.tindex[n11]
                    break
                k = n13[k]['parent']
                if k < 0:
                    nmap[i] = 0
                    break
        return nodes, nmap, origin

    def mesh(self, s13, tmpl, nmap, origin, textures, slot_names):
        s = jboy.Mesh()
        s.raw = bytes(tmpl.raw)
        s.u04 = 1
        s.u74 = 0
        s.shader = tmpl.shader
        s.vfmt = tmpl.vfmt
        s.palette = [nmap[p - 1] + 1 if p > 0 else p for p in s13['palette']]
        # (WWE '13 skin keeps shader data in the vertex colour - alpha 0, RGB 0x32 on some attires;
        # 2011 multiplies by it and draws its alpha as opacity: white, as 2011's own models)
        s.verts = [(0.1 * (v[0] - origin[0]), 0.1 * (v[1] - origin[1]), 0.1 * (v[2] - origin[2]), v[3], v[4], v[5], 0xFFFFFFFF)
                   for v in s13['verts']]
        vc = len(s.verts)
        nw = s13['nw']
        w = s13['wraw']
        s.weights = [[(tuple(w[8 * vc * j + 8 * k:8 * vc * j + 8 * k + 4]), struct.unpack_from('>f', w, 8 * vc * j + 8 * k + 4)[0])
                      for k in range(vc)] for j in range(nw)]
        s.uvs = list(s13['uvs'])
        if getattr(self, 'push', None):  # (a cloth piece over the body: a little out along its normals)
            d = self.push
            s.verts = [(v[0] + d * v[3], v[1] + d * v[4], v[2] + d * v[5], v[3], v[4], v[5], v[6]) for v in s.verts]
        s.strips = [(6, idx) for idx in s13['strips']]
        # parameters: the template's, its texture slots pointed at our names
        s.params = []
        for name, typ, val in tmpl.params:
            if typ == 0x0f and struct.unpack('>i', val[:4])[0] >= 0:   # (a slot the template leaves unbound stays so)
                want = slot_names.get(name)
                k = textures.index(want) if want in textures else -1
                val = struct.pack('>i', k) + val[4:]
            s.params.append((name, typ, val))
        # bounding sphere
        P = np.array([v[:3] for v in s.verts])
        c = (P.min(0) + P.max(0)) / 2
        r = float(np.sqrt(((P - c) ** 2).sum(1)).max())
        md = bytearray(s.raw)
        struct.pack_into('>I', md, 0x8C, tmpl.vfmt)
        struct.pack_into('>4f', md, 0xA4, float(c[0]), float(c[1]), float(c[2]), r)
        s.raw = bytes(md)
        return s

    def model(self, m13, tmpl_model, nodes, nmap, origin, textures, kind_of):
        m = jboy.Model()
        m.header = tmpl_model.header
        m.name = tmpl_model.name
        m.group = tmpl_model.group
        m.name_tail = getattr(tmpl_model, 'name_tail', b'')
        m.textures = textures
        m.nodes = nodes
        m.meshes = []
        names13 = [nd['name'] for nd in m13['nodes']]
        for k, s13 in enumerate(m13['meshes']):
            tmpl, slots = kind_of(s13)
            pal = [names13[p - 1] if 0 < p <= len(names13) else '' for p in s13['palette']]
            self.push = 0.08 if pal and sum(n.startswith('kami_b15') for n in pal) * 2 >= len(pal) else 0
            ms = self.mesh(s13, tmpl, nmap, origin, textures, slots)
            self.push = 0
            ms.u74 = k
            m.meshes.append(ms)
        return m

    def attire(self, kids13):
        """WWE '13 EMD children -> 2011 EMD children."""
        if 0x271a in kids13 and 0x2710 not in kids13:   # (2K16+ keep the hair at 0x271a)
            kids13 = dict(kids13)
            kids13[0x2710] = kids13[0x271a]
        body13 = read13(kids13[0])
        nodes, nmap, origin = self.nodes_for(body13)
        SKIN = {'texDiffuse': 'color', 'texNormal': 'normal', 'texOcclusion': 'mk', 'texTilingMap': 'noise'}
        EYE = {'texDiffuse': 'color', 'texCubeRefction': 'cubemap', 'texRefctionReg': 'color', 'texOcclusion': 'mk'}
        tex_body = ['color', 'normal', 'mk', 'noise', 'cubemap']

        def kind(s):
            if 'Reflect' in s['shader']:
                return self.p_eye, EYE
            return self.p_skin, SKIN
        out = {}
        body = self.model(body13, self.body, nodes, nmap, origin, tex_body, kind)
        out[0] = jboy.write(body)
        self.last = (body, body13, nodes, nmap, origin, tex_body)   # (gear(): the pieces join this body)
        # hair / mustache (transA) and eyelash (transB): same skeleton, by name
        for src, dst, tm, slots in ((0x2710, 0x2710, self.hair or self.body, {'texDiffuse': 'color', 'texNormal': 'normal', 'texOcclusion': 'mk', 'texTilingMap': 'noise'}),
                                    (0x4e22, 0x4e20, self.lash, {'texDiffuse': 'color'})):
            if src not in kids13 or kids13[src][:4] != b'JBOY':
                continue
            m13 = read13(kids13[src])
            hnodes, chain_idx = nodes, {}
            chains = hair_chains(m13) if src == 0x2710 and 0x3c in kids13 else {}
            if chains:   # (the hair moves: its chains, their 0x3C from WWE '13, the template's collision 0x3D)
                hnodes, chain_idx = self.chain_nodes(nodes, m13, origin, chains)
                self.hair_text = hair_text_11(kids13[0x3c], set(chains))
            sub_map = {}
            body_names = {b['name']: j for j, b in enumerate(body13['nodes'])}
            idx11 = {nd['name']: j for j, nd in enumerate(nodes)}
            for i, nd in enumerate(m13['nodes']):
                # a body bone by name; else the 2011 node the name maps to; else
                # the nearest ancestor that is one (hair chains hang off the head)
                if i in chain_idx:
                    sub_map[i] = chain_idx[i]
                    continue
                k = cloth_root(m13['nodes'], i)
                while k >= 0:
                    nm = m13['nodes'][k]['name']
                    if nm in body_names:
                        sub_map[i] = nmap[body_names[nm]]
                        break
                    if name13to11(nm) in idx11:
                        sub_map[i] = idx11[name13to11(nm)]
                        break
                    k = m13['nodes'][k]['parent']
                else:
                    sub_map[i] = idx11.get('atama', 0)
            tmpl = tm.meshes[-1]

            def hkind(s, t=tmpl, sl=slots, hair=dst == 0x2710):
                # (an opaque part in the hair model - 2K14 Hollywood Hogan's bandana, yCh_gear - is drawn like the
                # body's: the hair card's cut-out would cut it away with the alpha only hair cards get)
                if hair and not is_trans(s):
                    return self.p_skin, SKIN
                return t, sl
            out[dst] = jboy.write(self.model(m13, tm, hnodes, sub_map, origin, tex_body if dst == 0x2710 else ['color'],
                                             hkind))
        # extra models of the attire (2K14 Hollywood Hogan's entrance: T_shirt 0x1d and the feather boa 0x271a next
        # to the hair 0x2710): their meshes join the body, skinned to its bones by name, as gear() does
        # (each within the match's vertex memory - model_budget, 192 pages with the hair and lashes, no blood: the
        # opaque ones first; one that doesn't fit is left out: Hogan's feather boa, 21 pages, after the T-shirt)
        import model_budget
        extra = sorted(extra_models(kids13), key=lambda i: any(is_trans(s) for s in read13(kids13[i])['meshes']))
        self.extra, self.extra_left, self.props_left = [], [], []
        if extra:
            merged = jboy.read(out[0])
            others = sum(model_budget.attire_pages({k: out[k]})[0] for k in (0x2710, 0x4e20) if k in out)
            for i in extra:
                m13 = read13(kids13[i])
                if any(p in m13['name'].lower() for p in MOVE_PROPS):
                    self.props_left.append((i, m13['name']))
                    continue
                sub = _sub_map(m13, body13, nodes, nmap)
                pm = self.model(m13, self.body, nodes, sub, origin, tex_body,
                                lambda s: (self.p_hair, SKIN) if is_trans(s) else kind(s))
                trial = jboy.read(jboy.write(merged))
                for ms in pm.meshes:
                    ms.u74 = len(trial.meshes)
                    trial.meshes.append(ms)
                if model_budget.attire_pages({0: jboy.write(trial)})[0] + others > model_budget.LIMIT:
                    self.extra_left.append((i, m13['name']))
                    continue
                merged = trial
                self.extra.append((i, m13['name']))
            out[0] = jboy.write(merged)
            self.last = (merged,) + self.last[1:]
        extra = [i for i, _ in self.extra]
        # textures
        t13 = dict(tex_bundle_read(kids13[0xa]))
        color13 = tex_bundle_read(kids13[0xc])[0][1]
        mk = make_mk(t13.get('Mask') or t13.get('mask'))
        texs = [('normal', t13['normal']), ('noise', self.ttex['noise']), ('Mk', mk)]
        # the template's blood overlays (face / body / hair) and their hair textures
        texs += [(n, d) for n, d in self.ttex.items() if n.lower().startswith('bloodhair')]
        out[0xa] = tex_bundle_write(texs, self.t[0xa][:16])
        out[0xb] = self.t[0xb]
        # hair / eyelash cards: WWE '13 cuts them out with the normal map's
        # alpha, 2011 with the colour map's
        cut = []
        for src in (0x2710, 0x4e22) + tuple(extra):
            if src in kids13 and kids13[src][:4] == b'JBOY':
                for s13 in read13(kids13[src])['meshes']:
                    if not is_trans(s13):   # (opaque parts keep the plain alpha)
                        continue
                    for idx in s13['strips']:
                        cut += [(s13['uvs'][a_], s13['uvs'][b_], s13['uvs'][c_]) for a_, b_, c_ in strip_tris(idx)]
        out[0xc] = tex_bundle_write([('color', add_alpha(color13, t13['normal'], cut))], kids13[0xc][:16])
        for k in (0xd, 0xe, 0xf):
            if k in kids13:
                out[k] = kids13[k]
        lacks = [k for k in (0x28, 0x32, 0x50, 0x64) if k not in self.t]
        if lacks:   # (a "KeyError: 40" before: the game's own ch104.pac has them all - this one was replaced)
            raise RuntimeError('the template %s has no part %s (face animation / collision / offsets): it isn\'t the '
                             'game\'s own file - put the original back (the launcher\'s Install tab, Verify game files)'
                             % (self.tmpl_pac, ', '.join('0x%X' % k for k in lacks)))
        for k in (0x28, 0x32, 0x50, 0x64):
            out[k] = self.t[k]
        if getattr(self, 'hair_text', None):   # (hair chains: their parameters; body collision: the template's)
            out[0x3c] = self.hair_text
            self.hair_text = None
            if 0x3d in self.t:
                out[0x3d] = self.t[0x3d]
            else:
                import os as _os
                out[0x3d] = Conv(_os.path.join(_os.path.dirname(self.tmpl_pac), 'ch102.pac')).t[0x3d]
        for k in (0x7530, 0x7531, 0x7532):
            if k in self.t:
                out[k] = self.t[k]
        for k in (0xc8, 0xc9):  # the portrait (256 x 256 DXT5): the WWE '13 bust render if given
            out[k] = self.portrait or self.t[k]
        return out


def make_mk(mask_dds):
    """2011 'Mk': R = bruise mask (none), G, B, A from WWE '13's mask."""
    im = Image.open(io.BytesIO(mask_dds)).convert('RGBA')
    r, g, b, a = im.split()
    out = Image.merge('RGBA', (Image.new('L', im.size, 0), g, b, a))
    return dds_encode(out, 'DXT5')


def strip_tris(idx):
    out = []
    for k in range(len(idx) - 2):
        a, b, c = idx[k], idx[k + 1], idx[k + 2]
        if a == b or b == c or a == c:
            continue
        out.append((a, b, c) if k % 2 == 0 else (b, a, c))
    return out


def add_alpha(color_dds, normal_dds=None, cut_tris=()):
    """WWE '13 colour maps are DXT1 (no alpha); 2011's alpha is the
    specular level (a mid value from the brightness) and, on hair cards, the
    cut-out (WWE '13's normal map alpha there)."""
    from PIL import ImageDraw, ImageFilter
    im = Image.open(io.BytesIO(color_dds)).convert('RGB')
    a = im.convert('L').point(lambda v: 40 + v // 3)
    if normal_dds and cut_tris:
        W, H = im.size
        mask = Image.new('L', (W, H), 0)
        dr = ImageDraw.Draw(mask)
        for tri in cut_tris:
            dr.polygon([(u * W, v * H) for u, v in tri], fill=255)
        mask = mask.filter(ImageFilter.MaxFilter(5))
        na = Image.open(io.BytesIO(normal_dds)).convert('RGBA').split()[3].resize((W, H), Image.BILINEAR)
        a = Image.composite(na, a, mask)
    rgba = im.copy()
    rgba.putalpha(a)
    return dds_encode(rgba, 'DXT5')


def resize_rgba(img, size, method=None):
    """resize without Pillow's premultiplied alpha: RGB and A each on their own (the colour sheets'
    alpha is a specular level around 45 - premultiplied, the RGB of the smaller mips turns to specks)"""
    method = Image.BOX if method is None else method
    if img.mode != 'RGBA':
        return img.resize(size, method)
    rgb = img.convert('RGB').resize(size, method)
    rgb.putalpha(img.getchannel('A').resize(size, method))
    return rgb


def dds_encode(img, fourcc):
    w, h = img.size
    mips = int(math.log2(max(w, h))) + 1
    out = None
    for m in range(mips):
        im = img if m == 0 else resize_rgba(img, (max(1, w >> m), max(1, h >> m)))
        b = io.BytesIO()
        im.save(b, 'DDS', pixel_format=fourcc)
        v = b.getvalue()
        out = bytearray(v) if out is None else out + v[128:]
    struct.pack_into('<I', out, 8, struct.unpack_from('<I', out, 8)[0] | 0x20000)
    struct.pack_into('<I', out, 28, mips)
    # (the linear size = the top level's bytes, as Yuke's sheets have it: PIL writes a pitch there, and the
    # game reading it to step through the mips draws the smaller ones from the wrong place)
    struct.pack_into('<I', out, 20, max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * (8 if fourcc == 'DXT1' else 16))
    struct.pack_into('<I', out, 108, struct.unpack_from('<I', out, 108)[0] | 0x400008)
    return bytes(out)


GEAR_PIECES = range(0x5208, 0x5300)   # 2K16+ entrance-gear pieces: extra models in an attire's PACH
GEAR_DROP = (0xf, 0x64, 0xc8, 0xc9, 0x7530, 0x7531, 0x7532)   # (2011's own gear models have none of these: 0xf = a third mask sheet)


# limb bones aimed at the source's limb (Conv.nodes_for): bone -> the joint it points at
LIMB_AIM = {'%s_%s' % (s, a): '%s_%s' % (s, b) for s in 'lr'
            for a, b in (('ninoude', 'kote'), ('kote', 'te'), ('momo', 'sune'), ('sune', 'ashi'))}


def _align(a, b):
    """the smallest rotation turning unit a onto unit b"""
    v = np.cross(a, b)
    c = float(np.dot(a, b))
    if c > 1 - 1e-9:
        return np.eye(3)
    if c < -1 + 1e-9:
        q = np.array([1.0, 0, 0]) if abs(a[0]) < 0.9 else np.array([0, 1.0, 0])
        v = np.cross(a, q)
        v /= np.linalg.norm(v)
        return 2 * np.outer(v, v) - np.eye(3)
    V = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + V + V @ V / (1 + c)


def _euler_zyx(R):
    """R = Rz Ry Rx (rot()) -> (x, y, z)"""
    sy = max(-1.0, min(1.0, -float(R[2, 0])))
    y = math.asin(sy)
    if abs(sy) < 0.99999:
        return math.atan2(R[2, 1], R[2, 2]), y, math.atan2(R[1, 0], R[0, 0])
    return 0.0, y, math.atan2(-R[0, 1], R[1, 1])


def set_true_r(nd, r):
    """a jboy.py node dict's TRUE rotation (+32..+43): jboy reads 'r' at +28 (its [1], [2] = true x, y) and u40 =
    true z"""
    nd['r'] = (nd['r'][0], float(r[0]), float(r[1]))
    nd['u40'] = struct.unpack('>I', struct.pack('>f', float(r[2])))[0]


def is_trans(s13):
    """a see-through ('13 family yCh_trans / yCh_Aniso_trans ...) mesh: cut out with the alpha"""
    return 'trans' in s13['shader'].lower()


EXTRA_MODELS = range(0x10, 0x100)   # (JBOY children past the textures: 2K14 Hogan's 04 T_shirt = 0x1d)
# (extra models the source shows only in its own moves - Mankind '13's Mr. Socko, "sockko", child 0x1c, his Mandible
# Claw's: in the body 2011 draws it all match, so it is left out)
MOVE_PROPS = ('sock',)


def extra_models(kids13):
    """an attire's extra models the body / hair / lash conversion doesn't take: JBOY children 0x10-0xff, and 0x271a
    when 0x2710 is the hair (2K16+ keep their hair at 0x271a alone)"""
    out = [i for i in sorted(kids13) if i in EXTRA_MODELS and kids13[i][:4] == b'JBOY']
    if 0x271a in kids13 and 0x2710 in kids13 and kids13[0x271a][:4] == b'JBOY':
        out.append(0x271a)
    return out


def _sub_map(m13, body13, nodes, nmap):
    """a model's nodes -> the 2011 body's (by name; else the 2011 node the name maps to; else the nearest ancestor
    that is one; else atama) - as the hair's"""
    body_names = {b['name']: j for j, b in enumerate(body13['nodes'])}
    idx11 = {nd['name']: j for j, nd in enumerate(nodes)}
    out = {}
    for i in range(len(m13['nodes'])):
        k = cloth_root(m13['nodes'], i)
        while k >= 0:
            nm = m13['nodes'][k]['name']
            if nm in body_names:
                out[i] = nmap[body_names[nm]]
                break
            if name13to11(nm) in idx11:
                out[i] = idx11[name13to11(nm)]
                break
            k = m13['nodes'][k]['parent']
        else:
            out[i] = idx11.get('atama', 0)
    return out


def gear(conv, kids13, out):
    """2K16+: an attire's entrance-gear pieces (children 0x5208.., small see-through models on the attire's own
    sheets, skinned to its skeleton - Jericho 2K16's) -> the 2011 entrance-gear model (kind 4): the converted body
    with the first piece's meshes added (the others are variants of it), as 2011's own gear models (no face
    animation, portraits or blood). None when the attire has no piece."""
    pieces = sorted(i for i in kids13 if i in GEAR_PIECES and kids13[i][:4] == b'JBOY')
    if not pieces or not getattr(conv, 'last', None):
        return None
    body, body13, nodes, nmap, origin, tex_body = conv.last
    m13 = read13(kids13[pieces[0]])
    sub = _sub_map(m13, body13, nodes, nmap)
    tm = conv.hair or conv.body
    tmpl = tm.meshes[-1]   # (a see-through card's parameters: the hair's)
    slots = {'texDiffuse': 'color', 'texNormal': 'normal', 'texOcclusion': 'mk', 'texTilingMap': 'noise'}
    pm = conv.model(m13, tm, nodes, sub, origin, tex_body, lambda s: (tmpl, slots))
    merged = jboy.read(jboy.write(body))
    for ms in pm.meshes:
        ms.u74 = len(merged.meshes)
        merged.meshes.append(ms)
    g = {k: v for k, v in out.items() if k not in GEAR_DROP}
    g[0] = jboy.write(merged)
    gear.pieces = len(pieces)
    return g


def pack_kids(kids):
    return svrfmt.pach_write([(i, svrfmt.bpe_encode(kids[i], compress=True)) for i in sorted(kids)])


def convert(src, out, tmpl, portrait=None):
    conv = Conv(tmpl, portrait)
    convert.extras = []
    _, groups = ch_tool.epk8_read(open(src, 'rb').read())
    entries = []
    for typ, es in groups:
        for name, blob in es:
            if not name.strip(b'\0'):
                continue
            kids = {i: ch_tool.unpack(x) for i, x in svrfmt.pach_read(ch_tool.unpack(blob))}
            if 0 not in kids or kids[0][:4] != b'JBOY':
                print('skip', name)
                continue
            print('attire', name.decode(), '...')
            out11 = conv.attire(kids)
            for i, nm in conv.extra:
                print('  extra model %#x %s: in the body' % (i, nm))
            for i, nm in conv.extra_left:
                print('  extra model %#x %s: LEFT OUT (over the 192-page vertex memory)' % (i, nm))
            for i, nm in conv.props_left:
                print('  extra model %#x %s: LEFT OUT (a prop of its moves: 2011 would show it all match)' % (i, nm))
            convert.extras.extend('%s: %s left out (a prop its moves show - 2011 would draw it all match)' %
                                  (name.decode('latin1').strip('\x00 '), nm) for i, nm in conv.props_left)
            convert.extras.extend('%s: %s left out (vertex memory)' % (name.decode('latin1').strip('\x00 '), nm)
                                  for i, nm in conv.extra_left)
            if name.rstrip(b'\0 ').endswith(b'4'):
                # (the source's own entrance gear, kind 4: laid out as 2011's - Undertaker 00010104, Cena 00013904 -
                # with no face animation, portraits, blood overlays or third mask. Those (0x0f / 0x64 / 0xc8 / 0xc9
                # in the report) put the model over the match's vertex memory: the match start crashed.)
                for c in GEAR_DROP:
                    out11.pop(c, None)
                print('entrance gear', name.decode(), '(the source gear, laid out as 2011 gear)')
            entries.append((typ, name, pack_kids(out11)))
            g = gear(conv, kids, out11) if name.rstrip(b'\0').endswith(b'2') else None
            if g:   # (its entrance gear: <id><attire>4)
                gname = name.rstrip(b'\0')[:-1] + b'4'
                print('entrance gear', gname.decode(), '(%d piece(s), the first one)' % gear.pieces)
                entries.append((typ, gname, pack_kids(g)))
    write_epk8(tmpl, entries, out)
    print('wrote', out, os.path.getsize(out))


def write_epk8(tmpl, entries, out):
    """EPK8 with one EMD group holding the entries (laid out like 2011 ch pacs)."""
    t = open(tmpl, 'rb').read()
    header = bytearray(t[:0x800])
    index = bytearray(0x3800)
    struct.pack_into('<4sHH', index, 0, b'EMD ', 4 * len(entries), 0)  # (the count field is entries * 4)
    # copy the template's group record flags (bytes 6..12)
    index[6:12] = t[0x806:0x80C]
    body = bytearray()
    p = 12
    for typ, name, blob in entries:
        while len(body) % 0x800:
            body.append(0)
        index[p:p + 8] = name.ljust(8, b'\0')[:8]
        struct.pack_into('<II', index, p + 8, len(body) // 0x800, (len(blob) + 0xFF) // 0x100)
        body += blob
        p += 16
    while len(body) % 0x800:
        body.append(0)
    struct.pack_into('<I', header, 4, 12 + 16 * len(entries))  # the index size (the game reads only that much)
    struct.pack_into('<I', header, 8, len(body))
    open(out, 'wb').write(bytes(header) + bytes(index) + bytes(body) + bytes(0x800))


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('out')
    ap.add_argument('--template', required=True, help='a SvR2011 chNNN.pac: skeleton frames, face animation, cube map (ch104 works)')
    ap.add_argument('--portrait', help='256 x 256 DXT5 DDS (WWE 13 menuHD SSFC/<id>)')
    a = ap.parse_args()
    convert(a.src, a.out, a.template, a.portrait)
