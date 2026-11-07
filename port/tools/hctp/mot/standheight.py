"""Standing pelvis height in the motion data of both games (root y, 0.01 units, y down):
frames with both knees within 12 deg of straight and the lowest joint on the floor."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # (its own folder: the embeddable Python doesn't add it)
sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))  # (tools\: ymks.py)
import sys, numpy as np, ymks as Y, fk, hmot, s11mot
if len(sys.argv) != 2:
    raise SystemExit('usage: standheight.py <2011 motion bank file>  (HCTP_UNPACKED, HCTP_CH_PAC, SVR11_PAC set)')
B11 = sys.argv[1]
K = 12 / 360 * 65536

def collect(tracks, S, limit=150):
    hs = []
    for fr in tracks[:limit]:
        for r, A in fr[::2]:
            if abs(A[14][1]) < K and abs(A[18][1]) < K:
                W = S.pose([v * 0.01 for v in r], A)
                if abs(S.joints(W, fk.MOT_BONES)[:, 1].max()) < 0.3:
                    hs.append(r[1])
    return np.array(hs)

nH, _ = fk.hctp_nodes()
SH = fk.Skel(nH)
T, ents = hmot.read_bank(open(os.path.join(fk.need_env('HCTP_UNPACKED', 'the unpacked HCTP disc'), 'm', 'MOT_GAME', '00000.bin'), 'rb').read())
trH = [hmot.sample(e['data'], e['frames']) for e in ents if e['y'] == 0 and e['data'] and hmot.track_kind(e['data']) == 'char'][:150]
n11 = fk.svr11_nodes()
rH = {n['name']: n['r'] for n in nH}
for n in n11:
    if n['name'] in fk.MOT_BONES:
        n['r'] = rH[n['name']]
S11 = fk.Skel(n11)
B = Y.read_bank(open(B11, 'rb').read())
tr11 = []
for e in B.entries:
    if e['y'] == 0 and e['data'] and not e['share']:
        try:
            tr11.append(s11mot.sample(e['data'], e['frames']))
        except Exception:
            pass
    if len(tr11) >= 400:
        break
h = collect(trH, SH); g = collect(tr11, S11, 400)
print('HCTP (hogan skeleton) standing root y: n=%d median %.1f p10 %.1f p90 %.1f' % (len(h), np.median(h), np.percentile(h, 10), np.percentile(h, 90)))
print('2011 (ch104 skeleton) standing root y: n=%d median %.1f p10 %.1f p90 %.1f' % (len(g), np.median(g), np.percentile(g, 10), np.percentile(g, 90)))
# skeleton leg lengths (hip -> sole) at straight legs
for nm, S, Wf in (('hctp hogan', SH, None), ('2011 ch104 (motion bind)', S11, None)):
    W = S.pose([0, 0, 0], [[0, 0, 0]] * 21)
    J = S.joints(W, ['l_momo', 'l_sune', 'l_ashi', 'l_tsumasaki'])
    print(nm, 'hip y %.2f knee %.2f ankle %.2f toe %.2f' % tuple(J[:, 1]))
