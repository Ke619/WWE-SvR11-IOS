"""Which bind does SvR2011 motion data assume?  FK of stock 2011 character tracks on ch104 with
(a) ch104's own node rotations (A-pose: momo 15 deg, ninoude 15 deg, kote 20 deg) as the motion bind,
(b) HCTP's rest rotations (limbs 0) as the motion bind.  The right bind puts the lowest joint on the floor."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # (its own folder: the embeddable Python doesn't add it)
sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))  # (tools\: ymks.py)
import sys, numpy as np, ymks as Y, fk, s11mot
banks = sys.argv[1:] or ['78PK_bank.bin', r"C:\Users\Ranger\AppData\Local\Temp\claude\D--Xbox-Games-Ports-WWE-Smackdown-Vs-Raw-2011\dfe83e05-1f37-4854-b5ee-c656d2553e43\scratchpad\ymks\banks\m_MOT_BM01_1.bin"]
nH, _ = fk.hctp_nodes()
rH = {n['name']: n['r'] for n in nH}
own = fk.svr11_nodes()
hb = [dict(n) for n in own]
for n in hb:
    if n['name'] in fk.MOT_BONES:
        n['r'] = rH[n['name']]
for p in banks:
    B = Y.read_bank(open(p, 'rb').read())
    tr = []
    for e in B.entries:
        if e['y'] == 0 and e['data'] and not e['share'] and e['id'] != 0x7fff:
            try:
                tr.append(s11mot.sample(e['data'], e['frames']))
            except Exception:
                pass
        if len(tr) >= 80:
            break
    for nm, nodes in (('own A-pose bind', own), ('HCTP rest bind', hb)):
        S = fk.Skel(nodes)
        low = np.array([np.median([S.joints(S.pose([v * 0.01 for v in r], A), fk.MOT_BONES)[:, 1].max() for r, A in fr[::2]]) for fr in tr])
        print('%-40s %-16s tracks %d  median lowest joint y %.2f  within 0.5 of floor %d' % (p[-30:], nm, len(tr), np.median(low), (abs(low) < 0.5).sum()))
