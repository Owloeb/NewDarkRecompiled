#!/bin/sh
# harness/abi_test/run.sh: end-to-end check of lift.py --cache-regs: the calling-convention contract for indirect calls and
# imports, the callee-saved preservation proof (lift_preserve.py), their run-time check (RT_ABI_CHECK) and --abi-exempt.
# Run from the repository root:  sh harness/abi_test/run.sh      Needs gcc, python3 with pefile capstone unicorn ziglang.
#
# The test program (abitest.c) has normal code (test_good) and two negative controls that break the rules on purpose:
# test_bad calls a function that clobbers EBX/ESI through a pointer, test_smash calls one that overwrites its own saved
# EBX through a pointer the proof can't follow. Builds:
#   plain     lift.py without --cache-regs: the reference
#   old       --cache-regs --no-abi-calls --no-preserve: register caching as it was
#   new       --cache-regs: contract + preservation proof
#   check     new, compiled with -DRT_ABI_CHECK: must report both negative controls
#   exempt    new, relifted with every address the check reported in --abi-exempt: must match plain again
# The script checks the expectations itself and exits non-zero if one fails.
set -e
python3 harness/abi_test/x87_undecodable.py
T=harness/abi_test; W=${ABI_TEST_DIR:-build/abi_test}
mkdir -p $W
python3 -m ziglang cc -target x86-windows-gnu -mcpu=pentium -O2 -fno-inline-functions $T/abitest.c -o $W/abitest.exe
python3 - $W <<'PY'
import sys, pefile
w = sys.argv[1]; pe = pefile.PE(w + '/abitest.exe'); b = pe.OPTIONAL_HEADER.ImageBase
open(w + '/image.bin', 'wb').write(pe.get_memory_mapped_image())
ex = sorted((e.name.decode(), b + e.address) for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name.startswith(b'test_'))
open(w + '/args.txt', 'w').write(f"{b:x} " + ' '.join(f'{n}={a:x}' for n, a in ex) + '\n')
PY
lift() { out=$1; shift; python3 lift.py "$@" $W/abitest.exe ab $W/out_$out > $W/lift_$out.txt; }
lift plain
lift old --cache-regs --no-abi-calls --no-preserve
lift new --cache-regs
echo "-- register caching as it was:"; grep "call sites" $W/lift_old.txt
echo "-- with the contract and the preservation proof:"; grep "preservation\|call sites" $W/lift_new.txt
python3 -c "import json; print('static const char *imp_names[] = {' + ','.join('\"%s\"' % n for d, n in json.load(open('$W/out_new/ab_meta.json'))['imports']) + '};')" > $W/imps.h
CF="-O2 -fno-strict-aliasing -fwrapv -ffp-contract=off -w -Iruntime -I$W"
build() { gcc $CF "$@" -lm; }
build -I$W/out_plain $W/out_plain/*.c $T/run.c -o $W/run_plain
build -I$W/out_old $W/out_old/*.c $T/run.c -o $W/run_old
build -I$W/out_new $W/out_new/*.c $T/run.c -o $W/run_new
build -DRT_ABI_CHECK -I$W/out_new $W/out_new/*.c $T/run.c runtime/rt_abi_check.c -o $W/run_check
for m in plain old new check; do $W/run_$m $W/image.bin $(cat $W/args.txt) > $W/res_$m.txt 2>&1; done
# the workflow for a real violator: every address the check reported goes into --abi-exempt, relift
sed -n 's/^ABI VIOLATION: call at \([0-9a-f]*\) to \([0-9a-f]*\) .*/\1 \2/p' $W/res_check.txt > $W/exempt.txt
lift exempt --cache-regs --abi-exempt $W/exempt.txt
build -I$W/out_exempt $W/out_exempt/*.c $T/run.c -o $W/run_exempt
$W/run_exempt $W/image.bin $(cat $W/args.txt) > $W/res_exempt.txt 2>&1
echo "-- results (plain build):"; cat $W/res_plain.txt
echo "-- what the checked build reported:"; grep "ABI" $W/res_check.txt
echo "-- difftest (new build vs Unicorn):"
sh build.sh $W/out_new $W/obj_new $W/lib_new.so ab
UC_IGNORE_REG_BREAK=1 python3 harness/difftest.py $W/abitest.exe $W/out_new/ab_meta.json $W/lib_new.so --trials 4 --report $W/difftest.json | tail -1
python3 - $W <<'PY'
import sys, json
w = sys.argv[1]
def res(m):
    return {l.split(' = ')[0]: l.split(' = ')[1].strip() for l in open(f'{w}/res_{m}.txt') if ' = ' in l}
R = {m: res(m) for m in ('plain', 'old', 'new', 'check', 'exempt')}
pick = lambda m, t: {k: v for k, v in R[m].items() if k.startswith(t + '(')}
args = open(w + '/args.txt').read().split()[1:]
ex = {n: int(a, 16) for n, a in (x.split('=') for x in args)}
viol = [l for l in open(w + '/res_check.txt') if l.startswith('ABI VIOLATION')]
fails = [k for k, v in json.load(open(w + '/difftest.json'))['functions'].items() if v['verdict'] == 'FAIL']
checks = [
    ('test_good: same result in every build', all(pick(m, 'test_good') == pick('plain', 'test_good') for m in R)),
    ('old register caching matches plain', R['old'] == R['plain']),
    ('test_bad and test_smash: the new build differs from plain (the controls bite)',
     pick('new', 'test_bad') != pick('plain', 'test_bad') and pick('new', 'test_smash') != pick('plain', 'test_smash')),
    ('the checked build reports exactly two call sites (one per control)', len(viol) == 2),
    ('the checked build keeps the program consistent (same results as new)', R['check'] == R['new']),
    ('exempting the reported addresses gives plain results again', R['exempt'] == R['plain']),
    ('difftest: the only failures are the two controls', set(int(f, 16) for f in fails) <= {ex['test_bad'], ex['test_smash']}),
]
bad = 0
for name, ok in checks:
    print(('PASS  ' if ok else 'FAIL  ') + name); bad += not ok
print('abi_test: ' + ('all checks passed' if not bad else f'{bad} check(s) FAILED'))
sys.exit(1 if bad else 0)
PY
