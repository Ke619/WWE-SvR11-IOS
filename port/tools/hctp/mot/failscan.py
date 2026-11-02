import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # (its own folder: the embeddable Python doesn't add it)
sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))  # (tools\: ymks.py)
import hmot, ymks as Y, collections, sys
T,ents=hmot.read_bank(open(sys.argv[1],'rb').read())
c=collections.Counter(); ex={}
for e in ents:
    if e['y']>=10 and e['y']<100 or not e['data']: continue
    for f,r in hmot.records(e['data']):
        ev,pose=Y.split_record(r)
        if len(pose)<=1: continue
        try:
            b=hmot.decode_body(pose); bad=bool(b['trail'])
        except Exception as x: bad=True
        if bad:
            k=(e['y'],pose[0],len(pose)); c[k]+=1; ex.setdefault(k,(e['id'],e['x'],f,pose[:48].hex()))
for k,v in c.most_common(int(sys.argv[2]) if len(sys.argv)>2 else 15): print(k,v,ex[k])
