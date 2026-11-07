"""WWE '12 / '13 / 2K14-2K17 finger motion (YANM Biped finger tracks) -> SvR 2011 y2 / y3 finger tracks.

  python finger_retarget.py validate <extract or iso folder> --game-pac <2011 pac dir> <n> [<n> ...]
                                     [--game wwe13] [--svrmod exe] [--order bind|pre]
        converts the hands of entrance n and compares them with 2011's own entrance n (object 0, y2 / y3,
        every segment both have): angle error per joint (degrees), and the hand shape (mean curl of
        the four fingers) per frame, ours vs 2011's.

Module API (for wwe13_entrance.convert_tracks):
  finger_tracks(m, nframes, rt=None) -> {2: data, 3: data}   (m: yanm_decode.decode(..., sample=False))
  has_fingers(m) -> bool
  Retarget().angles(m, f) -> {k: (x, y, z) s8 steps} at a 60 Hz frame

2011 target (docs/HAND_POSES.md): y2 = right hand, y3 = left hand, one record per frame (count 0) of 15
sub-records [k][0][s8 x][s8 y][s8 z], k 0-14 left / 15-29 right (l_yubiFJ, F = finger 0-4, J = joint 0-2),
angle = s8 * 1.40625 degrees, R = Rz Ry Rx applied on top of the finger bone's bind rotation
(local = bind * R: the library's open hand 0 is all zeros against the ch104 bind, also for the thumb whose
bind is 43 degrees off); the first record carries hand_tracks.FIRST_EVENTS.

Method (both rigs are the same Biped hand, WWE '13 in cm and 2011 in 10 cm; the hand frames differ by a
fixed rotation - WWE '13's Bip Hand has r.x = -90 degrees where 2011's te has 0):
1. WWE '13 finger pose in its hand's frame: product of the finger chain's local rotations (track
   rotations replace the bind's, as the body).
2. A = the rotation taking WWE '13 hand coordinates to 2011's (Kabsch on the 30 bind finger joint
   positions x 0.1 of ch246 vs ch104), C_j = per joint bone-frame difference at bind
   ((A P13bind_j)^T P11bind_j, about identity up to the axis permutation A).
3. Target frame in 2011 hand coordinates T_j = A P13_j C_j; joints solved parents first against the
   already quantised chain: D = bind11_j^T (P11_parent^T T_j), angles = euler_zyx(D) / 1.40625 degrees.
Frames as the body: 2011 frame f samples WWE '13 frame 2f (last frame clamped); nframes = (frames13-1)//2.
A hand whose fingers no track moves (WWE '12 / 2K bodies without finger tracks) is left out of the result.
"""
import collections
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import numpy as np  # noqa: E402
import ymks  # noqa: E402
import yanm_decode as Y  # noqa: E402
from w13_retarget import rotm, euler_zyx  # noqa: E402

FIRST_EVENTS = bytes.fromhex('4005000000008900010000')    # (= hand_tracks.FIRST_EVENTS)
STEP = 2 * math.pi / 256.0                                 # s8 unit: 256 * 2pi / 65536

# 2011 ch104 finger nodes: name -> (t in te / parent space, r zyx radians); parents: yubiF0 -> te, yubiFJ -> yubiF(J-1)
FINGERS11 = {
    'l_yubi00': ((0.09280825, 0.1499929, -0.3009428), (0.2532623, 0.7518788, 0.362218)),
    'l_yubi01': ((0.641238, 0, 0), (-1.228575e-07, -0.4363322, 2.677201e-07)),
    'l_yubi02': ((0.3471527, -9.536743e-07, 0), (-5.016821e-15, -2.79396e-09, -2.235175e-07)),
    'l_yubi10': ((1.104639, -0.07640219, -0.4058225), (4.117128e-07, 0.1039879, 4.45531e-08)),
    'l_yubi11': ((0.362844, 4.768372e-07, -1.192093e-07), (3.725291e-09, -3.72529e-09, -6.938895e-18)),
    'l_yubi12': ((0.2799926, 0, 0), (-3.725289e-09, -1.862645e-08, -1.490116e-08)),
    'l_yubi20': ((1.12566, -0.1320763, -0.1049737), (-4.060567e-07, -8.381892e-09, 2.980232e-08)),
    'l_yubi21': ((0.4109759, 4.768372e-07, -5.960464e-08), (7.45058e-09, 2.700835e-08, -1.490116e-08)),
    'l_yubi22': ((0.316164, 0, -5.960464e-08), (7.450581e-09, -2.775559e-17, 7.450583e-09)),
    'l_yubi30': ((1.042147, -0.08520222, 0.1781939), (6.492649e-09, -0.1208366, -7.50531e-09)),
    'l_yubi31': ((0.3985758, 0, 2.384186e-07), (5.587938e-09, 1.071021e-08, 5.215406e-08)),
    'l_yubi32': ((0.306602, 0, -5.960464e-08), (-9.48086e-07, 2.7474e-08, -1.490115e-08)),
    'l_yubi40': ((0.9436665, -0.02419186, 0.4351287), (-7.459605e-09, -0.2084018, 4.095495e-08)),
    'l_yubi41': ((0.3049192, 0, 0.001343966), (1.303852e-08, 2.374873e-08, 2.235174e-08)),
    'l_yubi42': ((0.2356944, -4.768372e-07, -0.001344129), (-5.709007e-06, 1.070961e-08, -5.960475e-08)),
    'r_yubi00': ((0.09280872, -0.149992, -0.3009437), (-0.2532607, 0.7518799, -0.3622178)),
    'r_yubi01': ((0.6412376, -9.536743e-07, -9.536743e-07), (4.137486e-10, -0.4363322, 7.565289e-08)),
    'r_yubi02': ((0.3471527, 9.536743e-07, -4.768372e-07), (-1.862645e-08, 6.053594e-08, -1.490116e-08)),
    'r_yubi10': ((1.104639, 0.07640266, -0.4058241), (1.909743e-06, 0.1039887, 7.742133e-08)),
    'r_yubi11': ((0.362844, 0, -1.192093e-07), (-9.685746e-08, 4.507602e-07, 5.811454e-07)),
    'r_yubi12': ((0.2799931, 0, 2.384186e-07), (-7.823109e-08, -1.322478e-07, 7.450578e-08)),
    'r_yubi20': ((1.12566, 0.1320763, -0.1049749), (-3.725752e-09, 8.382134e-09, 6.181747e-05)),
    'r_yubi21': ((0.4109769, -2.574921e-05, 5.960464e-08), (-4.768372e-07, 1.303748e-08, -4.284084e-06)),
    'r_yubi22': ((0.3161654, -1.907349e-05, 0), (7.450579e-09, -1.024455e-08, -4.991889e-07)),
    'r_yubi30': ((1.042147, 0.08520174, 0.1781933), (1.631445e-06, -0.1208358, 2.24123e-06)),
    'r_yubi31': ((0.3985758, -9.536743e-07, -1.192093e-07), (-1.907349e-06, 5.587462e-09, -4.842877e-07)),
    'r_yubi32': ((0.306603, 0, -2.086163e-07), (3.20375e-07, 3.697349e-07, 3.501773e-07)),
    'r_yubi40': ((0.9436665, 0.02419043, 0.4351285), (1.528601e-06, -0.2084009, 1.940669e-06)),
    'r_yubi41': ((0.3049188, -4.768372e-07, -1.639128e-07), (-9.499519e-08, 3.948806e-07, -1.586973e-06)),
    'r_yubi42': ((0.2356949, -4.768372e-07, 2.980232e-08), (2.384185e-07, -2.761371e-07, 1.490112e-08)),
}
# WWE '13 ch246 (Mankind) finger nodes: name -> (t cm in parent space, r zyx radians); parents: FingerF -> Bip Hand
FINGERS13 = {
    'Bip L Finger0': ((0.928154, 3.008011, 1.499619), (1.570796, -0.2617981, 0.7851787)),
    'Bip L Finger01': ((6.412373, -0.0007629395, -0.001361847), (-0.0001325559, -0.4354581, 5.673192e-05)),
    'Bip L Finger02': ((3.471531, -0.0006561279, 0.002288818), (6.426125e-07, -2.65427e-07, 2.682208e-07)),
    'Bip L Finger1': ((11.04631, 4.056654, -0.7653046), (-8.660321e-07, -7.766268e-07, 0.1039888)),
    'Bip L Finger11': ((3.628471, -7.629395e-06, -0.0003204346), (1.128763e-06, 1.862648e-07, -4.759057e-07)),
    'Bip L Finger12': ((2.799931, -1.907349e-06, -0.0002288818), (1.676381e-07, 1.639128e-07, 1.611188e-07)),
    'Bip L Finger2': ((11.25647, 1.048124, -1.321762), (-1.262873e-06, -9.685737e-08, 2.104789e-07)),
    'Bip L Finger21': ((4.109749, 1.907349e-06, -0.0003356934), (1.713634e-07, 4.693865e-07, 3.371388e-07)),
    'Bip L Finger22': ((3.161617, -1.430511e-05, -0.0002593994), (1.788139e-07, 1.639128e-07, 1.406297e-07)),
    'Bip L Finger3': ((10.42137, -1.78354, -0.8527069), (-1.280868e-06, 1.613873e-07, -0.1208363)),
    'Bip L Finger31': ((3.985767, 1.716614e-05, -0.0002288818), (1.788139e-07, 1.862645e-07, 2.970919e-07)),
    'Bip L Finger32': ((3.066048, 1.525879e-05, -0.0001831055), (1.750887e-07, 1.788139e-07, 8.335338e-08)),
    'Bip L Finger4': ((9.436554, -4.352816, -0.2422791), (-8.896463e-07, -2.965419e-07, -0.2084012)),
    'Bip L Finger41': ((3.049255, -0.01344252, -0.0001678467), (1.806766e-07, 1.713633e-07, 2.630987e-07)),
    'Bip L Finger42': ((2.35696, 0.01344538, -0.0001220703), (1.713634e-07, 6.556511e-07, 2.118759e-07)),
    'Bip R Finger0': ((0.9282036, -3.008102, 1.499664), (-1.570798, -0.26169, -0.7851497)),
    'Bip R Finger01': ((6.412365, -1.525879e-05, -0.001541138), (1.822909e-06, -0.4354595, 1.510663e-06)),
    'Bip R Finger02': ((3.471569, 0.0001068115, 0.002195358), (8.009375e-08, 1.294538e-07, 1.639128e-07)),
    'Bip R Finger1': ((11.04628, -4.056733, -0.7651825), (-9.930907e-06, -1.559816e-06, -0.1039891)),
    'Bip R Finger11': ((3.62846, 1.907349e-05, -0.0003051758), (1.676381e-07, 7.450616e-09, -4.656613e-07)),
    'Bip R Finger12': ((2.799885, 9.536743e-07, -0.0002746582), (1.676381e-07, 2.235172e-08, 2.747402e-07)),
    'Bip R Finger2': ((11.25645, -1.048204, -1.321655), (-8.884816e-06, -3.054734e-07, 9.313357e-08)),
    'Bip R Finger21': ((4.109783, -1.716614e-05, -0.0003204346), (1.788139e-07, -3.054738e-07, 3.324821e-07)),
    'Bip R Finger22': ((3.161644, 2.861023e-06, -0.0002288818), (-3.017473e-07, 1.579523e-06, 1.571141e-06)),
    'Bip R Finger3': ((10.42134, 1.783446, -0.8527069), (-6.902413e-06, 6.56749e-07, 0.120837)),
    'Bip R Finger31': ((3.985718, 0, -0.0003204346), (1.788139e-07, -3.054738e-07, -4.190978e-09)),
    'Bip R Finger32': ((3.066048, 2.861023e-05, -0.0002441406), (-2.998848e-07, 1.601875e-06, 1.387205e-06)),
    'Bip R Finger4': ((9.4366, 4.35272, -0.2422791), (9.593376e-05, 6.502534e-05, 0.2084028)),
    'Bip R Finger41': ((3.049171, 2.765656e-05, 0), (-0.0001056828, -6.274129e-05, 1.115314e-06)),
    'Bip R Finger42': ((2.35696, -1.764297e-05, -0.0001068115), (1.750886e-07, 1.713634e-07, 2.840536e-08)),
}
HANDS = {3: ('l', 'L', 0), 2: ('r', 'R', 15)}             # 2011 sub-track -> (2011 side, Biped side, k base)


def name11(side, F, J):
    return '%s_yubi%d%d' % (side, F, J)


def name13(side, F, J):
    return 'Bip %s Finger%d%s' % (side, F, str(J) if J else '')


Y.add_names(list(FINGERS13))


def _track(m, nm):
    """m's track of a node (also when m was decoded before these names were registered: '#hash')"""
    tr = m['tracks'].get(nm)
    return tr if tr is not None else m['tracks'].get('#%08x' % Y.name_hash(nm))


def _s8(a):
    return max(-128, min(127, int(round(a / STEP))))


def _chain_bind(tab, names):
    """[(R local, t local)] of a finger chain -> per joint (R in hand coords, position in hand coords)"""
    R, p, out = np.eye(3), np.zeros(3), []
    for nm in names:
        t, r = tab[nm]
        p = p + R @ np.array(t, float)
        R = R @ rotm(r)
        out.append((R, p))
    return out


def _kabsch(P, Q):
    """rotation A minimising |A P_i - Q_i| (rows of P / Q are points)"""
    H = P.T @ Q
    U, S, Vt = np.linalg.svd(H)
    d = np.sign(np.linalg.det(Vt.T @ U.T))
    D = np.diag([1.0, 1.0, d])
    return Vt.T @ D @ U.T


class Retarget:
    """order: 'bind' (local = bind * R(angles), what the library poses fit) or 'pre' (R(angles) * bind)"""

    def __init__(self, order='bind', fingers13=FINGERS13, fingers11=FINGERS11):
        self.order = order
        self.f13, self.f11 = fingers13, fingers11
        self.hand = {}
        for y, (s11, s13, kb) in HANDS.items():
            b13 = {F: _chain_bind(fingers13, [name13(s13, F, J) for J in range(3)]) for F in range(5)}
            b11 = {F: _chain_bind(fingers11, [name11(s11, F, J) for J in range(3)]) for F in range(5)}
            P = np.array([0.1 * b13[F][J][1] for F in range(5) for J in range(3)])
            Q = np.array([b11[F][J][1] for F in range(5) for J in range(3)])
            A = _kabsch(P, Q)
            C = {(F, J): (A @ b13[F][J][0]).T @ b11[F][J][0] for F in range(5) for J in range(3)}
            B = {(F, J): rotm(fingers11[name11(s11, F, J)][1]) for F in range(5) for J in range(3)}
            fit = float(np.sqrt(np.mean(np.sum((P @ A.T - Q) ** 2, axis=1))))
            self.hand[y] = dict(A=A, C=C, B=B, s13=s13, kb=kb, fit=fit)

    def _local13(self, m, f, s13):
        """{(F, J): local rotation} of WWE '13 at 60 Hz frame f (bind where no track)"""
        out = {}
        for F in range(5):
            for J in range(3):
                nm = name13(s13, F, J)
                tr = _track(m, nm)
                R = None
                if tr is not None:
                    e = Y.sample_track(tr, f)
                    if e.get('rot') is not None:
                        R = np.array(Y.q_to_mat(Y.qnorm(e['rot'])))
                out[(F, J)] = R if R is not None else rotm(self.f13[nm][1])
        return out

    def hand_angles(self, m, f, y):
        """-> [(k, x, y, z)] * 15 (s8 steps) for sub-track y (2 right / 3 left) at 60 Hz frame f"""
        h = self.hand[y]
        L13 = self._local13(m, f, h['s13'])
        out = []
        for F in range(5):
            P13 = np.eye(3)
            P11 = np.eye(3)
            for J in range(3):
                P13 = P13 @ L13[(F, J)]
                T = h['A'] @ P13 @ h['C'][(F, J)]
                Lw = P11.T @ T
                B = h['B'][(F, J)]
                D = B.T @ Lw if self.order == 'bind' else Lw @ B.T
                a = [_s8(v) for v in euler_zyx(D)]
                out.append((h['kb'] + 3 * F + J, a[0], a[1], a[2]))
                R = rotm([v * STEP for v in a])
                P11 = P11 @ (B @ R if self.order == 'bind' else R @ B)
        return out

    def angles(self, m, f):
        return {y: self.hand_angles(m, f, y) for y in HANDS}


def has_fingers(m, y=None):
    """does the block animate any finger joint (of hand y, or either)?"""
    for yy, (s11, s13, kb) in HANDS.items():
        if y is not None and yy != y:
            continue
        for F in range(5):
            for J in range(3):
                if _track(m, name13(s13, F, J)) is not None:
                    return True
    return False


def _record(shape, first):
    pose = b''.join(struct.pack('BBbbb', k, 0, x, y, z) for k, x, y, z in shape)
    return ymks.join_record(FIRST_EVENTS if first else b'', pose)


def finger_tracks(m, nframes, rt=None, events=None):
    """YANM block m (yanm_decode.decode(..., sample=False)) -> {2: data, 3: data} of nframes frames
    (2011 frame f = WWE '13 frame 2f); hands without finger tracks are left out. events: optional
    {y: {frame: [event bytes]}} appended to the records' event blocks."""
    rt = rt or Retarget()
    last = m['frames'] - 1
    out = {}
    for y in HANDS:
        if not has_fingers(m, y):
            continue
        recs = []
        for f in range(max(1, nframes)):
            shape = rt.hand_angles(m, min(2 * f, last), y)
            rec = _record(shape, f == 0)
            extra = b''.join((events or {}).get(y, {}).get(f, []))
            if extra:
                ev, pose = ymks.split_record(rec)
                rec = ymks.join_record(ev + extra, pose)
            recs.append((f, rec))
        out[y] = ymks.build_stream(ymks.make_stream(recs, max(1, nframes)))
    return out


# ============================================================================ validation

def _sample(data, nframes):
    import svr06_hands
    return svr06_hands.sample(data, nframes)


def curl(fr, kb):
    """mean z of the four fingers' 3 joints (degrees, sign: + = closed for the left hand, - for the right)"""
    v = [fr[kb + 3 * F + J][2] for F in range(1, 5) for J in range(3) if (kb + 3 * F + J) in fr]
    return sum(v) / len(v) * 1.40625 if v else 0.0


def validate(evp13, evz13, number, game_pac, game='wwe13', order='bind'):
    import wwe13_entrance as W
    stock, _ = W.stock_2011(game_pac)
    if number not in stock:
        return ['2011 has no entrance %d' % number], None
    e11, b11 = stock[number]
    leaves = W.leaves_of(evz13)
    rt = Retarget(order)
    lines = ['== %d (%s) order %s; hand fit A: L %.3f R %.3f units' % (
        number, W.evp_name(e11), order, rt.hand[3]['fit'], rt.hand[2]['fit'])]
    agg = collections.defaultdict(list)
    tot = collections.Counter()
    for x, y, off in W.blocks(leaves.get(0, b'')):
        if y != 0:
            continue
        m = Y.decode(leaves[0], y=off, sample=False)
        nf = W.frames11(m)
        ours = finger_tracks(m, nf, rt)
        for yy, kb in ((2, 15), (3, 0)):
            g = b11.find(25000, x, yy)
            if not g or not g['data'] or yy not in ours:
                tot['skipped'] += 1
                continue
            n = min(nf, g['frames'])
            a, b = _sample(ours[yy], n), _sample(g['data'], n)
            errs = collections.defaultdict(list)
            ca, cb = [], []
            for fa, fb in zip(a, b):
                for k in range(kb, kb + 15):
                    if k in fa and k in fb:
                        errs[k].append(max(abs((p - q + 128) % 256 - 128) for p, q in zip(fa[k], fb[k])) * 1.40625)
                ca.append(curl(fa, kb))
                cb.append(curl(fb, kb))
            ca, cb = np.array(ca), np.array(cb)
            corr = float(np.corrcoef(ca, cb)[0, 1]) if ca.std() > 1e-6 and cb.std() > 1e-6 else float('nan')
            med = np.median([np.median(v) for v in errs.values()])
            for k, v in errs.items():
                agg[k] += v
            agg['curl'] += list(np.abs(ca - cb))
            sign = -1 if yy == 2 else 1
            closed_a, closed_b = sign * ca > 45, sign * cb > 45
            tot['frames'] += len(ca)
            tot['shape agree'] += int(np.sum(closed_a == closed_b))
            lines.append('  x%-2d y%d frames %d: joint error median %.1f deg; curl ours %.0f..%.0f 2011 %.0f..%.0f deg, '
                         'mean |diff| %.1f, corr %.2f; fist(>45) agree %.0f%%' % (
                             x, yy, n, med, sign * ca.min() if sign > 0 else sign * ca.max(),
                             sign * ca.max() if sign > 0 else sign * ca.min(),
                             sign * cb.min() if sign > 0 else sign * cb.max(),
                             sign * cb.max() if sign > 0 else sign * cb.min(),
                             float(np.mean(np.abs(ca - cb))), corr, 100.0 * np.mean(closed_a == closed_b)))
    return lines, (agg, tot)


def main(argv):
    import wwe13_entrance as W
    a = list(argv)
    if len(a) < 2 or a[0] != 'validate':
        print(__doc__)
        return 2
    gp = W._opt(a, '--game-pac')
    game = W._opt(a, '--game', 'wwe13')
    order = W._opt(a, '--order', 'bind')
    sv = W.find_svrmod(W._opt(a, '--svrmod')) if game != 'wwe12' else None
    evp, evz = W.read_source(W._Folder(a[1]), sv, game)
    allagg = collections.defaultdict(list)
    alltot = collections.Counter()
    for n in a[2:]:
        n = int(n)
        if n not in evp or n not in evz:
            print('== %d: not in the source' % n)
            continue
        lines, res = validate(evp[n], evz[n], n, gp, game, order)
        print('\n'.join(lines))
        if res:
            for k, v in res[0].items():
                allagg[k] += v
            alltot.update(res[1])
    if alltot['frames']:
        print('ALL: frames %d, fist/open agree %.1f%%, curl mean |diff| %.1f deg; joint error median / 95%%:' % (
            alltot['frames'], 100.0 * alltot['shape agree'] / alltot['frames'], float(np.mean(allagg['curl']))))
        print('  ' + ' '.join('%d:%.0f/%.0f' % (k, np.median(v), np.percentile(v, 95))
                              for k, v in sorted((k, v) for k, v in allagg.items() if k != 'curl')))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
