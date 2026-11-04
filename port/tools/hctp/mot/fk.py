"""Forward kinematics for HCTP / SvR2011 skeletons driven by 21-bone motion poses.
Column-vector convention: R(r) = Rz(z) Ry(y) Rx(x)  (== HCTP objMatrix::RotZYX, rows transposed;
== SvR2011 YMKs/YMBs).  Motion bone local rotation = R_bind * R_motion [C, CMotionHokan::ActionLink
multiplies motion x bind with VU0 row-vector matrices]."""
import math, sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
ANG = 2 * math.pi / 65536.0
MOT_BONES = ['root', 'koshi', 'mune', 'kubi', 'atama', 'l_sakotsu', 'l_ninoude', 'l_kote', 'l_te',
             'r_sakotsu', 'r_ninoude', 'r_kote', 'r_te', 'l_momo', 'l_sune', 'l_ashi', 'l_tsumasaki',
             'r_momo', 'r_sune', 'r_ashi', 'r_tsumasaki']


def rot(r):
    x, y, z = r
    cx, sx, cy, sy, cz, sz = math.cos(x), math.sin(x), math.cos(y), math.sin(y), math.cos(z), math.sin(z)
    Rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    Rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    return Rz @ Ry @ Rx


def euler_of(R):
    """inverse of rot(): R = Rz Ry Rx -> (x, y, z) radians"""
    sy = -R[2, 0]
    sy = max(-1.0, min(1.0, sy))
    y = math.asin(sy)
    if abs(sy) < 0.999999:
        x = math.atan2(R[2, 1], R[2, 2])
        z = math.atan2(R[1, 0], R[0, 0])
    else:      # gimbal: put everything in z
        x = 0.0
        z = math.atan2(-R[0, 1], R[1, 1])
    return x, y, z


class Skel:
    """nodes: list of dict(name, t, r, parent) in parent-before-child order"""
    def __init__(self, nodes):
        self.nodes = nodes
        self.index = {n['name']: i for i, n in enumerate(nodes)}
        self.bind = [rot(n['r']) for n in nodes]
        self.mot = {self.index[b]: k for k, b in enumerate(MOT_BONES) if b in self.index}

    def pose(self, root_t, angles, mode='bind_motion', motion_rot=None):
        """root_t: model units (x, y, z); angles: [21][3] s16 values (or None)
        motion_rot: optional {bone k: 3x3} overriding the Euler angles. -> list of 4x4 world"""
        W = []
        for i, n in enumerate(self.nodes):
            R = self.bind[i]
            k = self.mot.get(i)
            if k is not None:
                M = motion_rot[k] if motion_rot and k in motion_rot else rot([a * ANG for a in angles[k]])
                R = R @ M if mode == 'bind_motion' else M if mode == 'motion' else M @ R
            T = np.eye(4)
            T[:3, :3] = R
            T[:3, 3] = root_t if (n['parent'] < 0 and k == 0) else n['t']
            W.append(T if n['parent'] < 0 else W[n['parent']] @ T)
        return W

    def joints(self, W, names):
        return np.array([W[self.index[n]][:3, 3] for n in names])


def need_env(name, what):
    v = os.environ.get(name)
    if not v:
        raise SystemExit('set %s to %s' % (name, what))
    return v


def hctp_nodes(pac_path=None, entry=0):
    pac_path = pac_path or need_env('HCTP_CH_PAC', "the HCTP disc's PAC\\CH.PAC")
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
    import hpac, yobj
    d = open(pac_path, 'rb').read()
    g, nm, o, s = hpac.dpac(d)[entry]
    p = dict(hpac.pac(d[o:o + s]))
    m = yobj.read(p[2], apply_nodes=False)
    return [dict(name=n['name'], t=tuple(n['t']), r=tuple(n['r']), parent=n['parent']) for n in m.nodes], m.name


def svr11_nodes(path=None):
    path = path or os.path.join(need_env('SVR11_PAC', 'the SvR 2011 pac folder'), 'ch', 'ch104.pac')
    import skel11
    return [dict(name=n['name'], t=tuple(n['t']), r=tuple(n['r']), parent=n['parent']) for n in skel11.nodes(path)]
