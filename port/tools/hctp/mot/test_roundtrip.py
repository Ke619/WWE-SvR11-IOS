"""Round trips.
  HCTP:  every record of every char/object track: decode -> encode == original pose bytes.
  2011:  ymks (decoder/encoder from session dfe83e05, copied here): write_bank(read_bank) == bank and
         every unpacked character pose decode_body -> encode_body == bytes, for 78PK's GMGB bank and
         the m.pac / evt banks in dfe83e05's ymks/banks folder (optional, --all)."""
import sys, glob, collections, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # (its own folder: the embeddable Python doesn't add it)
sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))  # (tools\: ymks.py)
import hmot, ymks as Y

H = [r"D:/Xbox Games Ports/HCTP Extract/_unpacked/m/MOT_GAME/0000%d.bin" % b for b in (0, 1, 2, 3, 4, 6, 7)] + \
    [r"D:/Xbox Games Ports/HCTP Extract/_unpacked/bg/stg_0030/50102.bin"]
S11 = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '..',
                   'dfe83e05-1f37-4854-b5ee-c656d2553e43', 'scratchpad', 'ymks', 'banks')


def hctp():
    tot = collections.Counter()
    for p in H:
        T, ents = hmot.read_bank(open(p, 'rb').read())
        forced = 'obj' if p.endswith('50102.bin') else None
        for e in ents:
            if not e['data']:
                continue
            k = forced or hmot.track_kind(e['data'], e['y'])
            if k not in ('char', 'obj'):
                tot['skipped ' + str(k)] += 1
                continue
            dec, enc = (hmot.decode_body, hmot.encode_body) if k == 'char' else (hmot.decode_obj, hmot.encode_obj)
            for f, r in hmot.records(e['data']):
                ev, pose = Y.split_record(r)
                if len(pose) <= 1:
                    continue
                ok = enc(dec(pose)) == pose
                tot[(k, ok)] += 1
    print('HCTP pose records decode->encode:', dict(tot))


def svr11(all_banks=False):
    paths = ['78PK_bank.bin']
    if all_banks:
        paths += sorted(glob.glob(os.path.join(S11, 'm_*.bin')))
    tot = collections.Counter()
    for p in paths:
        raw = open(p, 'rb').read()
        if raw[:4] != b'YMKs':
            continue
        B = Y.read_bank(raw)
        tot['bank identical'] += Y.write_bank(B) == raw
        tot['banks'] += 1
        for e in B.entries:
            if e['share'] or not e['data'] or e['y'] > 1:
                continue
            try:
                st = Y.parse_stream(e['data'])
            except ValueError:
                tot['stream error'] += 1
                continue
            tot['stream identical'] += Y.build_stream(st) == e['data']
            try:
                if Y.is_packed_motion(e['data'], B.T):
                    continue
            except Exception:
                pass
            for f, r in st.records():
                ev, pose = Y.split_record(r)
                if len(pose) <= 1:
                    continue
                try:
                    ok = Y.encode_body(Y.decode_body(pose)) == pose
                except Exception:
                    ok = False
                tot[('110-byte 0x7d' if len(pose) == 110 and pose[0] == 0x7d else 'other', ok)] += 1
    print('2011:', dict(tot))


if __name__ == '__main__':
    hctp()
    svr11('--all' in sys.argv)
