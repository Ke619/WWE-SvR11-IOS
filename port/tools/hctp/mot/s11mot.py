"""SvR2011 YMKs character track -> per-frame (root, angles) using ymks.py (unpacked m.pac form)."""
import collections
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # (its own folder: the embeddable Python doesn't add it)
sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))  # (tools\: ymks.py)
import ymks as Y


def keys(data, T=None):
    out = []
    for f, r in Y.parse_stream(data).records():
        ev, pose = Y.split_record(r)
        if len(pose) <= 1:
            continue
        b = Y.decode_body(pose)
        for s in b['subs']:
            if 'bones' in s or s['op'] in (0x7e, 0x7f):
                out.append((f + s.get('count', 0) + 1, f, s.get('root') if s['op'] not in (0x7b,) else None, s.get('bones', {}), s['op']))
    return out


def sample(data, nframes=None):
    import hmot
    ks = keys(data)
    # reuse the HCTP sampler logic (identical semantics)
    return _sample(ks, nframes)


def _sample(ks, nframes=None):
    if nframes is None:
        nframes = max([k[0] for k in ks] + [1]) + 1
    chans = collections.defaultdict(list)
    for tgt, f, r, bones, op in ks:
        if r is not None:
            for a in range(3):
                chans[('r', a)].append((f, tgt, float(r[a])))
        for b, v in bones.items():
            for ai, ax in enumerate('xyz'):
                if ax in v:
                    chans[(b, ai)].append((f, tgt, float(v[ax])))
    cur, idx, state, frames = {}, {c: 0 for c in chans}, {}, []
    for fr in range(nframes):
        for c, lst in chans.items():
            while idx[c] < len(lst) and lst[idx[c]][0] <= fr:
                f, tgt, v = lst[idx[c]]
                v0 = cur.get(c, v)
                if c[0] != 'r':
                    v = v0 + ((v - v0 + 32768) % 65536 - 32768)
                state[c] = (f, v0, tgt, v)
                idx[c] += 1
            if c in state:
                f0, v0, f1, v1 = state[c]
                t = 1.0 if f1 <= f0 else min(1.0, (fr - f0) / float(f1 - f0))
                cur[c] = v0 + (v1 - v0) * t
        frames.append(([cur.get(('r', a), 0.0) for a in range(3)],
                       [[cur.get((b, a), 0.0) for a in range(3)] for b in range(21)]))
    return frames
