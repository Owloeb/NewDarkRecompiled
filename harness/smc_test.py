#!/usr/bin/env python3
"""Targeted differential test for self-modifying code.

Runs every function that writes to code or contains a patched instruction with many random states,
and counts trials in which a patched instruction actually executed *after* its field was written
in that trial (i.e. the translated runtime-read path was exercised with a live patch value).

usage: smc_test.py <pe> <meta.json> <lib.so> <gen dir> [--trials N]
"""
import sys, os, re, json, glob, random, collections, argparse
sys.path.insert(0, os.path.dirname(__file__))
from difftest import Env, HEAP, STACK
import struct
from unicorn import UC_HOOK_CODE
from unicorn import UC_HOOK_MEM_WRITE
from multiprocessing import Pool


def smc_functions(gendir, meta):
    lo_hi = [(a, a + n) for (nm, a, n, ch) in meta['sections'] if ch & 0x20000000]
    is_code = lambda v: any(a <= v < b for a, b in lo_hi)
    fns = set()
    for f in glob.glob(os.path.join(gendir, '*_f*.c')):
        cur = None
        for line in open(f):
            m = re.match(r'void \w+_([0-9a-f]{8})\(CPU', line)
            if m: cur = int(m.group(1), 16); continue
            if '/* SMC' in line:
                fns.add(cur)
            for w in re.finditer(r'WR(?:8|16|32)\(0x([0-9a-f]+)u', line):
                if is_code(int(w.group(1), 16)): fns.add(cur)
    return sorted(x for x in fns if x is not None)


_env = None
def _small_gen(E):
    """inputs biased to small, sane values so renderer spans terminate: heap = pointers into heap or
    small ints, args/regs likewise."""
    orig = E.gen
    def gen(rng):
        regs, heap, stack = orig(rng)
        h = bytearray(heap)
        for k in range(0, len(h), 4):
            x = rng.random()
            if x < 0.4: v = HEAP[0] + (rng.randrange(0, HEAP[1] - 0x1000) & ~3)
            elif x < 0.9: v = rng.randrange(0, 24)
            else: v = rng.randrange(0, 0x10000)
            struct.pack_into('<I', h, k, v)
        st = bytearray(stack)
        sp = regs[4] - STACK[0]
        for k in range(1, 40):
            v = struct.unpack_from('<I', st, sp + 4 * k)[0]
            if not (HEAP[0] <= v < HEAP[0] + HEAP[1]): struct.pack_into('<I', st, sp + 4 * k, rng.randrange(0, 24))
        regs = [r if (HEAP[0] <= r < HEAP[0] + HEAP[1]) or k == 4 or k == 5 else rng.randrange(0, 24) for k, r in enumerate(regs)]
        return regs, bytes(h), bytes(st)
    E.gen = gen


def _smc_globals(gendir, fns):
    """absolute data addresses read by the SMC-family functions (renderer state globals)."""
    want = set(fns); out = set()
    for f in glob.glob(os.path.join(gendir, '*_f*.c')):
        cur = None
        for line in open(f):
            m = re.match(r'void \w+_([0-9a-f]{8})\(CPU', line)
            if m: cur = int(m.group(1), 16); continue
            if cur in want:
                for g in re.finditer(r'uint32_t ea = \(uint32_t\)\(0x([0-9a-f]+)u\);', line):
                    out.add(int(g.group(1), 16))
    return sorted(out)


def _prepatch(E, meta):
    """after each state load, fill every patched field with a live value (same bytes on both sides)."""
    sites = {int(k, 16): v for k, v in meta['smc_sites'].items()}
    branch = {int(k, 16): [int(x, 16) for x in v] for k, v in meta['smc_branch'].items()}
    orig = E.load_state
    gl = [a for a in getattr(E, 'smc_globals', []) if not any(lo <= a < hi for lo, hi in E.code_ranges)]
    def load_state(heap, stack):
        orig(heap, stack)
        rng = random.Random(struct.unpack_from('<I', heap, 0)[0])
        for a in gl:
            v = HEAP[0] + 0x1000 + (rng.randrange(0, 0x30000) & ~3) if rng.random() < 0.5 else rng.randrange(0, 32)
            E.put(a & ~3, struct.pack('<I', v))
        for va, fields in sites.items():
            for f in fields:
                a = va + f['off']
                if va in branch:
                    i = E.cs.disasm(bytes(E.mu.mem_read(va, 16)), va).__next__()
                    t = rng.choice(branch[va]); v = (t - (va + i.size)) & ((1 << (8 * f['size'])) - 1)
                elif f['kind'] == 'disp':
                    v = HEAP[0] + 0x1000 + rng.randrange(0, 0x30000)
                elif f['size'] == 1:
                    v = rng.randrange(0, 24)            # shift counts
                else:
                    v = rng.choice([0xFF, 0xFFFF, 0x3F, 0x7F00, 0xFFFF0000, 0x00FF00FF, rng.getrandbits(32)])
                E.put(a, int(v).to_bytes(f['size'], 'little'))
    E.load_state = load_state


def _init(pe, meta, lib):
    global _env
    _env = Env(pe, meta, lib)
    if os.environ.get('SMC_SMALL'): _small_gen(_env)
    if os.environ.get('SMC_PREPATCH'):
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
        _env.cs = Cs(CS_ARCH_X86, CS_MODE_32)
        m = json.load(open(meta))
        _env.smc_globals = _smc_globals(os.environ.get('SMC_GEN', 'out/ss2'), smc_functions(os.environ.get('SMC_GEN', 'out/ss2'), m))
        _prepatch(_env, m)
    m = json.load(open(meta))
    _env.patched_insns = {int(k, 16): v for k, v in m['smc_sites'].items()}


def _job(args):
    entry, n = args
    E = _env
    res = collections.Counter()
    live = 0
    for t in range(n):
        E.new_uc()
        written, hits = set(), [0]
        def onw(mu, acc, addr, size, val, ud):
            written.update(range(addr, addr + size))
        def onx(mu, addr, size, ud):
            for f in E.patched_insns.get(addr, ()):
                if addr + f['off'] in written or os.environ.get('SMC_PREPATCH'): hits[0] += 1
        for lo, hi in E.code_ranges:
            E.mu.hook_add(UC_HOOK_MEM_WRITE, onw, begin=lo, end=hi - 1)
        for a in E.patched_insns:
            E.mu.hook_add(UC_HOOK_CODE, onx, begin=a, end=a)
        r, d = E.trial(entry, entry * 1000 + 500 + t)
        res[r] += 1
        if hits[0] and r in ('PASS', 'PASS_SW', 'FAULT_PASS'):
            live += 1
        if hits[0] and r not in ('PASS', 'PASS_SW', 'FAULT_PASS'):
            res['LIVE_' + r] += 1
    return entry, dict(res), live


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('pe'); ap.add_argument('meta'); ap.add_argument('lib'); ap.add_argument('gendir')
    ap.add_argument('--trials', type=int, default=48); ap.add_argument('--workers', type=int, default=2)
    a = ap.parse_args()
    meta = json.load(open(a.meta))
    fns = smc_functions(a.gendir, meta)
    print(f'{len(fns)} functions write code or contain patched instructions')
    tot = collections.Counter(); live_total = 0; live_fns = 0; bad = []
    with Pool(a.workers, _init, (a.pe, a.meta, a.lib)) as pool:
        for entry, res, live in pool.imap_unordered(_job, [(f, a.trials) for f in fns]):
            tot.update(res); live_total += live; live_fns += live > 0
            if any(k in res for k in ('MISMATCH', 'OUTCOME_MISMATCH', 'FAULT_STATE_MISMATCH', 'UNKNOWN_SMC')):
                bad.append((hex(entry), res))
    print('trials:', dict(tot))
    print(f'passing trials that executed a live-patched instruction: {live_total} across {live_fns} functions')
    for b in bad: print('  ', b)
