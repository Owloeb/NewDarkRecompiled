import json,collections,sys
r=json.load(open(sys.argv[1]))
om=collections.Counter(); ex={}
for f,v in r['functions'].items():
    for k,d in v['trials']:
        if k in('OUTCOME_MISMATCH','MISMATCH','FAULT_STATE_MISMATCH'):
            key=(d['u'][0],d['c'][0]) if k=='OUTCOME_MISMATCH' else (k,)
            om[key]+=1; ex.setdefault(key,[]).append(f)
for k,v in om.most_common(): print(v,k, 'funcs:',len(set(ex[k])), sorted(set(ex[k]))[:6])
