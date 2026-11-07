"""Prop (object) motion state: final/held values per frame for CSD3MotionWeapon tracks.
bone k = node named root / boneNN (tblSD3WEP); translation of bone = rest t + 0x7b offset * 0.01."""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # (its own folder: the embeddable Python doesn't add it)
sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))  # (tools\: ymks.py)
import numpy as np
import hmot, ymks as Y, fk

NAMES = ['root'] + ['bone%02d' % i for i in range(1, 20)]


def states(data, decode=hmot.decode_obj):
    """-> list of (frame, {k: (x,y,z) s16 rot}, {k: (x,y,z) offset}, root) after each record (targets)"""
    rot, tr, root, out = {}, {}, (0, 0, 0), []
    for f, r in hmot.records(data) if decode is hmot.decode_obj else Y.parse_stream(data).records():
        ev, p = Y.split_record(r)
        if len(p) <= 1:
            continue
        for s in decode(p)['subs']:
            if s['op'] == 0x7b:
                tr[s['ref']] = s['trans']
            elif 'root' in s and s['op'] in (0x7e, 0x7d):
                root = s['root']
            if 'bones' in s:
                for k, v in s['bones'].items():
                    rot[k] = (v['x'], v['y'], v['z'])
        out.append((f, dict(rot), dict(tr), root))
    return out


def fk_obj(nodes, rot, tr, root):
    idx = {n['name']: i for i, n in enumerate(nodes)}
    kof = {idx[n]: k for k, n in enumerate(NAMES) if n in idx}
    W = []
    for i, n in enumerate(nodes):
        R = fk.rot(n['r'])
        t = np.array(n['t'], float)
        k = kof.get(i)
        if k is not None:
            if k in rot:
                R = R @ fk.rot([a * fk.ANG for a in rot[k]])
            if k in tr:
                t = t + np.array(tr[k]) * 0.01
            if k == 0:
                t = t + np.array(root) * 0.01
        M = np.eye(4); M[:3, :3] = R; M[:3, 3] = t
        W.append(M if n['parent'] < 0 else W[n['parent']] @ M)
    return W


if __name__ == '__main__':
    import yobj
    m = yobj.read(open(sys.argv[1], 'rb').read(), apply_nodes=False)
    T, ents = hmot.read_bank(open(sys.argv[2], 'rb').read())
    e = next(e for e in ents if e['id'] == int(sys.argv[3]))
    st = states(e['data'])
    for f, rot, tr, root in (st[0], st[len(st) // 2], st[-1]):
        W = fk_obj(m.nodes, rot, tr, root)
        print('frame', f, ' '.join('%s y=%.1f x=%.1f' % (n['name'], W[i][1, 3], W[i][0, 3]) for i, n in enumerate(m.nodes) if n['name'].startswith('bone')))
