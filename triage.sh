#!/bin/sh
U=/root/.claude/uploads/db1c45d1-6b36-5ebc-86bf-cd811c35e29a/577c1577-SS2.exe
# triage.sh <verdict> <outfile>
python3 - "$1" > /tmp/triage_list.txt <<'P'
import json,sys
r=json.load(open('nd.json'))['functions']
bad={'FAULT_STATE_MISMATCH','UNKNOWN_SMC','INCONCLUSIVE'}
for a,d in r.items():
    if d['verdict']==sys.argv[1]:
        for i,t in enumerate(d['trials']):
            if t[0] in bad: print(a,i); break
        else: print(a,0)
P
: > $2
while read a t; do
  echo "== $a trial $t: $(UC_IGNORE_REG_BREAK=1 timeout 150 python3 harness/tracecmp.py $U out/nd/nd_meta.json build/libndt.so $a $t 2>&1 | grep -E 'first divergence|no divergence|Error|rror' | head -1 | cut -c1-240)" >> $2
done < /tmp/triage_list.txt
echo finished >> $2
