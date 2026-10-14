#!/usr/bin/env python3
"""harness/abi_test/csmith_fuzz.py: differential fuzzing of lift.py --cache-regs with random C programs (Csmith).

    python3 harness/abi_test/csmith_fuzz.py [--seeds 1-40] [--csmith /path/to/csmith] [--jobs N]

With --float the programs do floating-point math, which the 32-bit target compiles to x87 code: the test for
lift_x87.py (x87 stack in C locals). Each Csmith program is compiled for 32-bit Windows (Zig, x87-free like the game's era of compilers: -mcpu=pentium, at
-O1/-O2/-Os in turn), lifted twice (plain, and --cache-regs with the calling-convention contract and the preservation
proof), and run through harness/abi_test/run.c. The plain build is the reference (the recompiler's default output, which
is verified against an x86 emulator); the --cache-regs build must print the same checksum, and the same build compiled
with -DRT_ABI_CHECK must report no violation. Csmith programs are deliberately full of calls, pointers into the stack,
and register pressure, so they exercise exactly what the register summaries and preservation proofs claim.

Needs: Csmith (https://github.com/csmith-project/csmith; build it, then pass --csmith or put it on PATH), gcc, and the
Python packages lift.py needs plus ziglang. Exit code 0 when every program agrees."""
import argparse, concurrent.futures, os, re, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HERE = os.path.dirname(os.path.abspath(__file__))
PY = sys.executable

# guest-side glue: Csmith's minimal runtime calls printf("checksum = %llx\n", x) at the end of main(); this printf only
# records the value, and an exported entry point runs the program and returns the checksum folded to 32 bits
STUB = r'''
#include <stdarg.h>
volatile unsigned long long fuzz_checksum;
int printf(const char *f, ...) {
    va_list ap; va_start(ap, f);
    if (f[0] == 'c' && f[1] == 'h') fuzz_checksum = va_arg(ap, unsigned long long);
    va_end(ap); return 0;
}
int csmith_main(void);
__declspec(dllexport) __declspec(noinline) int test_csmith(int seed) {
    (void)seed; csmith_main();
    return (int)(fuzz_checksum ^ (fuzz_checksum >> 32));
}
#undef main
int main(void) { return test_csmith(0) & 0xFF; }   /* only so the exe links; the test host calls test_csmith */
'''


def run(cmd, timeout=600, **kw):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=timeout, **kw)


def one(seed, a):
    w = os.path.join(a.build, f's{seed}'); shutil.rmtree(w, ignore_errors=True); os.makedirs(w)
    opt = ['-O1', '-O2', '-Os'][seed % 3]
    src = run([a.csmith, '--no-argc', '--seed', str(seed), '--max-funcs', str(a.max_funcs)] + (['--float'] if a.float else [])).stdout
    open(os.path.join(w, 'prog.c'), 'w').write(src)
    open(os.path.join(w, 'stub.c'), 'w').write(STUB)
    exe = os.path.join(w, 'prog.exe')
    r = run([PY, '-m', 'ziglang', 'cc', '-target', 'x86-windows-gnu', '-mcpu=pentium', opt, '-w', '-fno-inline-functions',
             '-DCSMITH_MINIMAL', '-Dmain=csmith_main', '-I', a.include, '-Wno-error=incompatible-pointer-types', '-Wno-error=int-conversion', '-Wno-error=incompatible-function-pointer-types',
             os.path.join(w, 'prog.c'), os.path.join(w, 'stub.c'), '-o', exe])
    if r.returncode: return seed, opt, 'skip (does not compile)', None
    import pefile
    pe = pefile.PE(exe); base = pe.OPTIONAL_HEADER.ImageBase
    open(os.path.join(w, 'image.bin'), 'wb').write(pe.get_memory_mapped_image())
    va = next(base + e.address for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name == b'test_csmith')
    args = [os.path.join(w, 'image.bin'), f'{base:x}', f'test_csmith={va:x}']
    res, stats = {}, None
    for mode, flags in (('plain', []), ('new', ['--cache-regs'])):
        out = os.path.join(w, 'out_' + mode)
        r = run([PY, 'lift.py'] + flags + [exe, 'ab', out])
        if r.returncode: return seed, opt, f'lift failed ({mode})', r.stderr[-400:]
        if mode == 'new': stats = [l for l in r.stdout.splitlines() if 'call sites' in l or 'preservation' in l]
        import json
        imps = json.load(open(os.path.join(out, 'ab_meta.json')))['imports']
        open(os.path.join(out, 'imps.h'), 'w').write('static const char *imp_names[] = {' + ','.join('"%s"' % n for d, n in imps) + '};\n')
        for chk in ([False] if mode == 'plain' else [False, True]):
            exe_h = os.path.join(w, f'run_{mode}{"_check" if chk else ""}')
            cc = ['gcc', '-O1', '-fno-strict-aliasing', '-fwrapv', '-ffp-contract=off', '-w', '-Iruntime', '-I' + out] + (['-DRT_ABI_CHECK'] if chk else []) + \
                 [os.path.join(out, f) for f in sorted(os.listdir(out)) if f.endswith('.c')] + [os.path.join(HERE, 'run.c')] + \
                 ([os.path.join(ROOT, 'runtime', 'rt_abi_check.c')] if chk else []) + ['-lm', '-o', exe_h]
            r = run(cc)
            if r.returncode: return seed, opt, f'host build failed ({mode})', r.stderr[-400:]
            try:
                r = run([exe_h] + args, timeout=a.timeout)
            except subprocess.TimeoutExpired:
                res[mode + ('_check' if chk else '')] = 'timeout'; continue
            res[mode + ('_check' if chk else '')] = r.stdout + r.stderr
    p, n, c = res.get('plain'), res.get('new'), res.get('new_check')
    if p == 'timeout': return seed, opt, 'skip (reference run times out)', None
    nums = lambda s: [l for l in (s or '').splitlines() if ' = ' in l]
    if 'FAULT' in (p or '') or not nums(p): return seed, opt, 'skip (reference run faults: lifter coverage, not this change)', p[-300:] if p else None
    if nums(n) != nums(p): return seed, opt, 'MISMATCH', f'plain: {nums(p)}\nnew:   {nums(n) or n[-300:]}'
    if nums(c) != nums(p): return seed, opt, 'MISMATCH (checked build)', f'plain: {nums(p)}\ncheck: {nums(c) or c[-300:]}'
    v = [l for l in c.splitlines() if 'ABI VIOLATION' in l]
    if v: return seed, opt, 'VIOLATION', '\n'.join(v[:5])
    return seed, opt, 'ok', stats


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--seeds', default='1-40', help='range a-b [1-40]')
    ap.add_argument('--csmith', default=shutil.which('csmith') or 'csmith')
    ap.add_argument('--include', help="Csmith's include directory [next to the binary: ../include]")
    ap.add_argument('--max-funcs', type=int, default=25)
    ap.add_argument('--float', action='store_true', help='floating-point programs (Csmith --float): x87 code for lift_x87.py')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument('--timeout', type=int, default=20, help='seconds per run of a lifted program [20]')
    ap.add_argument('--build', default=os.path.join(ROOT, 'build', 'csmith_fuzz'))
    a = ap.parse_args()
    a.build = os.path.abspath(a.build)
    if not a.include: a.include = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(a.csmith))), 'include')
    lo, hi = (int(x) for x in a.seeds.split('-'))
    tally, bad = {}, 0
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for seed, opt, verdict, info in ex.map(lambda s: one(s, a), range(lo, hi + 1)):
            key = verdict.split(' (')[0]; tally[key] = tally.get(key, 0) + 1
            print(f'seed {seed:4d} {opt}: {verdict}', flush=True)
            if verdict in ('MISMATCH', 'MISMATCH (checked build)', 'VIOLATION') or verdict.startswith('lift failed') or verdict.startswith('host build'):
                bad += 1
                if info: print('    ' + str(info).replace('\n', '\n    '))
    print('csmith_fuzz:', ', '.join(f'{k} {v}' for k, v in sorted(tally.items())))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
