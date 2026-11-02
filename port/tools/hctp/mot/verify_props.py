"""Prop motions: convert every act of an HCTP object bank (default room 30's 50102) to 2011 object
tracks and check (1) each 2011 record parses with 2011's object grammar (0x75 = s16-root masked,
0x7e root, 0x7b, single bone; seen on 5679 GMGB object records) with no trail, (2) the held
rotation / translation / root state after every record equals the HCTP source, (3) FK of the
cardboard stack (danboru, 50101/01243) at the end of act 33 puts every box on the floor."""
import sys, struct
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # (its own folder: the embeddable Python doesn't add it)
sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))  # (tools\: ymks.py)
import hmot, ymks as Y, hctp2ymks as C, objpose

PB = r"D:/Xbox Games Ports/HCTP Extract/_unpacked/bg/stg_0030/50102.bin"
DANBORU = r"D:/Xbox Games Ports/HCTP Extract/_unpacked/bg/stg_0030/50101/01243.yobj"


def decode11_obj(p):
    """2011 object pose: 0x75 [count][s16 root][mask][xyz s16...] (= HCTP 0x7a); others as HCTP"""
    b = bytearray(p)
    i = 0
    while i < len(b) - 1:
        if b[i] == 0x75:
            b[i] = 0x7a
        d = hmot.decode_obj(bytes(b[i:]))
        s = d['subs'][0]
        i += len(hmot.encode_obj({'subs': [s], 'trail': b''}))
    d = hmot.decode_obj(bytes(b))
    return d


def main():
    pb = sys.argv[1] if len(sys.argv) > 1 else PB
    T, ents = hmot.read_bank(open(pb, 'rb').read())
    ids = sorted({e['id'] for e in ents if e['data']})
    B, done, rep = C.convert(pb, [(i, i) for i in ids], objects=True)
    B2 = Y.read_bank(Y.write_bank(B))
    ok_parse = ok_state = n = 0
    for e in B2.entries:
        if e['id'] == 0x7fff or not e['data']:
            continue
        src = next(x for x in ents if x['id'] == e['id'] and x['x'] == e['x'] and x['y'] == e['y'])
        n += 1
        try:
            s11 = objpose.states(e['data'], decode=decode11_obj)
            ok_parse += all(not decode11_obj(Y.split_record(r)[1])['trail'] for f, r in Y.parse_stream(e['data']).records()
                            if len(Y.split_record(r)[1]) > 1)
        except Exception as ex:
            print('parse failed', e['id'], ex)
            continue
        sh = objpose.states(src['data'])
        ok_state += sh == s11
    print('prop tracks %d: 2011 grammar parse %d/%d, held state identical %d/%d' % (n, ok_parse, n, ok_state, n))
    import yobj
    m = yobj.read(open(DANBORU, 'rb').read(), apply_nodes=False)
    e = B2.find(33, 0, 0)
    f, rot, tr, root = objpose.states(e['data'], decode=decode11_obj)[-1]
    W = objpose.fk_obj(m.nodes, rot, tr, root)
    ys = [W[i][1, 3] for i, nd in enumerate(m.nodes) if nd['name'].startswith('bone')]
    print('danboru act 33 (converted) last key frame %d: box centre y %s (floor contact = -2.5)' % (f, ' '.join('%.1f' % y for y in ys)))
    print('report', dict(rep))


if __name__ == '__main__':
    main()
