#!/bin/sh
# harness/abi_test/run.sh: end-to-end check of lift.py --cache-regs and its calling-convention contract (RT_ABI_CHECK).
# Builds a small x86 Windows test program with Zig, lifts it four ways, runs each through a Linux test host and runs the
# Unicorn differential test. Run from the repository root:  sh harness/abi_test/run.sh
# Needs: gcc, python3 with pefile capstone unicorn ziglang.
#
# Expected: test_good gives the same numbers in every build. test_bad calls a function that clobbers EBX/ESI through a
# pointer (breaking the convention on purpose): "contract" diverges from "plain"; "check" reports one ABI VIOLATION (and,
# keeping the program consistent, gives the same numbers as "contract"); "exempt", relifted with that reported call site
# in --abi-exempt, matches "plain" again. difftest's only FAIL in the contract build is test_bad.
set -e
T=harness/abi_test; W=${ABI_TEST_DIR:-build/abi_test}
mkdir -p $W
python3 -m ziglang cc -target x86-windows-gnu -mcpu=pentium -O2 -fno-inline-functions $T/abitest.c -o $W/abitest.exe
python3 - $W <<'PY'
import sys, pefile
w = sys.argv[1]; pe = pefile.PE(w + '/abitest.exe'); b = pe.OPTIONAL_HEADER.ImageBase
open(w + '/image.bin', 'wb').write(pe.get_memory_mapped_image())
ex = {e.name.decode(): b + e.address for e in pe.DIRECTORY_ENTRY_EXPORT.symbols}
open(w + '/args.txt', 'w').write(f"{b:x} {ex['test_good']:x} {ex['test_bad']:x}\n")
PY
for m in plain old contract; do
    case $m in
        plain)    python3 lift.py $W/abitest.exe ab $W/out_$m > $W/lift_$m.txt ;;
        old)      python3 lift.py --cache-regs --no-abi-calls $W/abitest.exe ab $W/out_$m > $W/lift_$m.txt ;;
        contract) python3 lift.py --cache-regs $W/abitest.exe ab $W/out_$m > $W/lift_$m.txt ;;
    esac
done
grep "cache-regs" $W/lift_old.txt $W/lift_contract.txt | sed "s|$W/lift_||"
python3 -c "import json; print('static const char *imp_names[] = {' + ','.join('\"%s\"' % n for d, n in json.load(open('$W/out_contract/ab_meta.json'))['imports']) + '};')" > $W/imps.h
CF="-O2 -fno-strict-aliasing -fwrapv -w -Iruntime -I$W"
gcc $CF -I$W/out_plain    $W/out_plain/*.c    $T/run.c -lm -o $W/run_plain
gcc $CF -I$W/out_old      $W/out_old/*.c      $T/run.c -lm -o $W/run_old
gcc $CF -I$W/out_contract $W/out_contract/*.c $T/run.c -lm -o $W/run_contract
gcc $CF -DRT_ABI_CHECK -I$W/out_contract $W/out_contract/*.c $T/run.c runtime/rt_abi_check.c -lm -o $W/run_check
for m in plain old contract check; do echo "== $m"; $W/run_$m $W/image.bin $(cat $W/args.txt) 2>&1 | tee $W/out_$m.txt; done
# the workflow for a real violator: take the reported call site, exempt it, relift
sed -n 's/^ABI VIOLATION: call at \([0-9a-f]*\) .*/\1/p' $W/out_check.txt > $W/exempt.txt
python3 lift.py --cache-regs --abi-exempt $W/exempt.txt $W/abitest.exe ab $W/out_exempt > $W/lift_exempt.txt
gcc $CF -I$W/out_exempt $W/out_exempt/*.c $T/run.c -lm -o $W/run_exempt
echo "== exempt (call site $(cat $W/exempt.txt))"; $W/run_exempt $W/image.bin $(cat $W/args.txt)
echo "== difftest (contract build vs Unicorn)"
sh build.sh $W/out_contract $W/obj_contract $W/lib_contract.so ab
UC_IGNORE_REG_BREAK=1 python3 harness/difftest.py $W/abitest.exe $W/out_contract/ab_meta.json $W/lib_contract.so --trials 4 --report $W/difftest.json | tail -2
python3 -c "import json; r = json.load(open('$W/difftest.json'))['functions']; print('FAIL:', [k for k, v in r.items() if v['verdict'] == 'FAIL'], '(expected: test_bad only, ' + open('$W/args.txt').read().split()[2] + ')')"
