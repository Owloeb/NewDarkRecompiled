#!/usr/bin/env python3
"""Find the first instruction where recompiled C and Unicorn diverge for one trial.

usage: tracecmp.py <pe> <meta.json> <trace lib.so> <entry hex> <trial index>
"""
import sys, os, ctypes, math, random
sys.path.insert(0, os.path.dirname(__file__))
from difftest import *
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from unicorn import UC_HOOK_CODE


class TraceEnt(ctypes.Structure):
    _fields_ = [('va', ctypes.c_uint32), ('r', ctypes.c_uint32 * 8), ('top', ctypes.c_uint32), ('st', ctypes.c_double * 8)]


def main():
    pe, meta, lib, entry, t = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4], 16), int(sys.argv[5])
    env = Env(pe, meta, lib)
    if os.environ.get('SMC_PREPATCH'):
        import smc_test, json
        smc_test._small_gen(env)
        env.cs = Cs(CS_ARCH_X86, CS_MODE_32)
        m = json.load(open(meta))
        gd = os.environ.get('SMC_GEN', 'out/ss2')
        env.smc_globals = smc_test._smc_globals(gd, smc_test.smc_functions(gd, m))
        smc_test._prepatch(env, m)
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    env.new_uc()
    utr = []
    def onx(mu, addr, size, ud):
        if len(utr) < 200000:
            fsw = mu.reg_read(UC_X86_REG_FPSW); top = (fsw >> 11) & 7
            phys = [mu.reg_read(UC_X86_REG_FP0 + i) for i in range(8)]
            utr.append((addr, [mu.reg_read(x) for x in REGS], top, [f80_to_double(*phys[(top + i) & 7]) for i in range(8)]))
    env.mu.hook_add(UC_HOOK_CODE, onx)
    rng = random.Random(entry * 1000 + t)
    regs, heap, stack = env.gen(rng)
    env.load_state(heap, stack)
    print('unicorn:', env.run_u(entry, regs)[:2])
    env.load_state(heap, stack)
    print('C:      ', env.run_c(entry, regs)[:2])
    n = ctypes.c_int.in_dll(env.lib, 'hx_trace_n').value
    ctr = (TraceEnt * 200000).in_dll(env.lib, 'hx_trace')
    print(f'trace lengths: unicorn {len(utr)}, C {n}')

    def depth(top):
        return (8 - top) & 7
    for k in range(min(n, len(utr))):
        c = ctr[k]; ua, ur, utop, ust = utr[k]
        diffs = []
        if c.va != ua: diffs.append(f'pc u={ua:x} c={c.va:x}')
        for j in range(8):
            if ur[j] != c.r[j]: diffs.append(f'{RNAMES[j]} u={ur[j]:x} c={c.r[j]:x}')
        if utop != c.top: diffs.append(f'top u={utop} c={c.top}')
        else:
            for j in range(depth(utop)):
                a, b = ust[j], c.st[j]
                if not (a == b or (math.isnan(a) and math.isnan(b))): diffs.append(f'st{j} u={a!r} c={b!r}')
        if diffs:
            print(f'first divergence before step {k}:', '; '.join(diffs))
            for kk in range(max(0, k - 8), k + 1):
                a = utr[kk][0]
                i = next(md.disasm(bytes(env.mu.mem_read(a, 16)), a))
                print(f'   {kk:6d} {a:08x} {i.mnemonic} {i.op_str}   u.top={utr[kk][2]} u.st0..2={utr[kk][3][:3]}  c.st0..2={list(ctr[kk].st)[:3]}')
            return
    print('no divergence within common trace prefix')


main()
