"""Convert HCTP motions and check them with the 2011 reader (ymks):
  1. the 2011 bank re-reads/re-writes byte-identically and every converted record decodes
     with ymks (no trailing bytes);
  2. per frame the sampled pose (root + 21 bones x 3 axes) equals the HCTP pose exactly;
  3. FK of the converted motion on the 2011 skeleton (ch104, motion bind = HCTP rest) puts the
     lowest joint on the floor like the HCTP motion on HCTP's Hogan.
  4. optional PNG: stick figures, HCTP (Hogan) left vs converted on ch104 right.

  python verify_convert.py <hctp bank> <id,id,...|first:N> [--png out.png --frames 0,10,20]
"""
import sys, collections
import numpy as np
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # (its own folder: the embeddable Python doesn't add it)
sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))  # (tools\: ymks.py)
import hmot, ymks as Y, fk, s11mot, hctp2ymks as C


def skel11_motion_bind():
    nH, _ = fk.hctp_nodes()
    rH = {n['name']: n['r'] for n in nH}
    n11 = fk.svr11_nodes()
    for n in n11:
        if n['name'] in fk.MOT_BONES:
            n['r'] = rH[n['name']]
    return fk.Skel(nH), fk.Skel(n11)


def main():
    a = sys.argv[1:]
    bank = a[0]
    T, ents = hmot.read_bank(open(bank, 'rb').read())
    if a[1].startswith('first:'):
        n = int(a[1][6:])
        ids = []
        for e in ents:
            if e['y'] == 0 and e['data'] and hmot.track_kind(e['data'], 0) == 'char' and e['id'] not in ids:
                ids.append(e['id'])
            if len(ids) >= n:
                break
    else:
        ids = [int(x) for x in a[1].split(',')]
    B, done, rep = C.convert(bank, [(i, i) for i in ids])
    raw = Y.write_bank(B)
    B2 = Y.read_bank(raw)
    st = collections.Counter()
    st['bank rewrite identical'] = Y.write_bank(B2) == raw
    SH, S11 = skel11_motion_bind()
    lowH, low11 = [], []
    maxd_ang, maxd_root = 0.0, 0.0
    for e in B2.entries:
        if e['id'] == 0x7fff or not e['data']:
            continue
        src = next(x for x in ents if x['id'] == e['id'] and x['x'] == e['x'] and x['y'] == e['y'])
        kind = hmot.track_kind(src['data'], src['y'])
        for f, r in Y.parse_stream(e['data']).records():
            ev, pose = Y.split_record(r)
            if ev:
                ok = Y.parse_events(ev, B2.T)[1]
                st['event blocks parsing with the 2011 T table: %s' % ok] += 1
            if len(pose) > 1:
                if kind == 'char':
                    d = Y.decode_body(pose)
                    st['2011 char records decoded, no trail'] += not d['trail']
                    st['110-byte 0x7d records'] += (len(pose) == 110 and pose[0] == 0x7d)
        if kind != 'char':
            continue
        fh = hmot.sample(src['data'], src['frames'] or None)
        f11 = s11mot.sample(e['data'], src['frames'] or None)
        st['char tracks compared'] += 1
        for (rh, Ah), (r1, A1) in zip(fh, f11):
            maxd_root = max(maxd_root, max(abs(x - y) for x, y in zip(rh, r1)))
            maxd_ang = max(maxd_ang, max(abs(Ah[b][k] - A1[b][k]) for b in range(21) for k in range(3)))
        if e['y'] == 0:
            lowH.append(np.median([SH.joints(SH.pose([v * 0.01 for v in r], A), fk.MOT_BONES)[:, 1].max() for r, A in fh]))
            low11.append(np.median([S11.joints(S11.pose([v * 0.01 for v in r], A), fk.MOT_BONES)[:, 1].max() for r, A in f11]))
    for k, v in st.items():
        print('  %-40s %s' % (k, v))
    print('  max |pose difference| HCTP vs converted: angles %.3f (s16 units), root %.4f (0.01 units)' % (maxd_ang, maxd_root))
    if lowH:
        lowH, low11 = np.array(lowH), np.array(low11)
        print('  lowest joint per y0 track (median of frames): HCTP/Hogan median %.2f (%d/%d within 0.5 of the floor);'
              ' converted/ch104 median %.2f (%d/%d)' % (np.median(lowH), (abs(lowH) < 0.5).sum(), len(lowH),
                                                     np.median(low11), (abs(low11) < 0.5).sum(), len(low11)))
    print('  converter report:', dict(rep))
    if '--png' in a:
        frames = [int(x) for x in a[a.index('--frames') + 1].split(',')] if '--frames' in a else [0, 5, 10, 15, 20]
        e = next(x for x in B2.entries if x['id'] == ids[0] and x['y'] == 0)
        src = next(x for x in ents if x['id'] == ids[0] and x['y'] == 0 and x['x'] == e['x'])
        render(SH, S11, hmot.sample(src['data'], src['frames'] or None), s11mot.sample(e['data'], src['frames'] or None),
               frames, a[a.index('--png') + 1], 'HCTP %d' % ids[0])


def render(SH, S11, fh, f11, frames, out, title):
    from PIL import Image, ImageDraw
    cell = 220
    img = Image.new('RGB', (cell * len(frames), cell * 2 + 20), 'white')
    dr = ImageDraw.Draw(img)
    dr.text((4, 2), title + ': top = HCTP on Hogan (CH.PAC 0), bottom = converted on SvR2011 ch104 (front view, y down)', fill='black')
    for row, (S, fr) in enumerate(((SH, fh), (S11, f11))):
        for col, fi in enumerate(frames):
            if fi >= len(fr):
                continue
            r, A = fr[fi]
            W = S.pose([v * 0.01 for v in r], A)
            ox, oy = col * cell + cell / 2, 20 + row * cell + cell - 15
            sc = 9.0
            root = W[0][:3, 3]
            dr.line((col * cell, oy, col * cell + cell, oy), fill=(200, 200, 200))
            for i, n in enumerate(S.nodes):
                p = n['parent']
                if p < 0 or 'yubi' in n['name'] or n['name'] not in fk.MOT_BONES + ['l_momo_x', 'r_momo_x', 'kubi_x', 'l_ninoude_x', 'r_ninoude_x', 'l_kote_x', 'r_kote_x', 'l_kote_x2', 'r_kote_x2']:
                    continue
                a_, b_ = W[p][:3, 3], W[i][:3, 3]
                col_ = (200, 0, 0) if n['name'].startswith('l_') else (0, 0, 200) if n['name'].startswith('r_') else (0, 0, 0)
                dr.line((ox + (a_[0] - root[0]) * sc, oy + a_[1] * sc, ox + (b_[0] - root[0]) * sc, oy + b_[1] * sc), fill=col_, width=2)
            dr.text((col * cell + 4, 20 + row * cell + 4), 'f%d' % fi, fill='black')
    img.save(out)
    print('  wrote', out)


if __name__ == '__main__':
    main()
