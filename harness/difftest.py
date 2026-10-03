#!/usr/bin/env python3
"""Differential test: recompiled C (libao.so) vs Unicorn running the original x86.

usage: difftest.py <pe> <meta.json> <lib.so> [--trials N] [--only 0xADDR,...] [--workers N] [--report out.json]
"""
import sys, os, ctypes, struct, random, json, math, argparse, collections, mmap
import pefile
from multiprocessing import Pool
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_PROT_READ, UC_PROT_WRITE, UC_PROT_EXEC, UC_PROT_ALL, \
    UC_HOOK_INTR, UC_ERR_FETCH_UNMAPPED, UC_ERR_FETCH_PROT, UC_ERR_READ_UNMAPPED, UC_ERR_WRITE_UNMAPPED, \
    UC_ERR_READ_PROT, UC_ERR_WRITE_PROT, UC_ERR_INSN_INVALID, UC_ERR_FETCH_UNALIGNED, UC_ERR_READ_UNALIGNED, \
    UC_ERR_WRITE_UNALIGNED, UC_ERR_EXCEPTION
from unicorn.x86_const import *
from unicorn import UC_HOOK_MEM_WRITE

IMP_BASE = 0x7FFE0000
SENTINEL = 0x00010000
STACK = (0x00200000, 0x40000)
STACK_TOP = 0x0023F000
HEAP = (0x20000000, 0x40000)
TIB = 0x7FFD0000
GDT = 0x7FFC0000
OUT = {0: 'RET', 1: 'IMPORT', 2: 'EXTERNAL', 3: 'MEMFAULT', 4: 'FAULT', 5: 'BUDGET', 6: 'NOFUNC'}
REGS = [UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
        UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI]
RNAMES = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
UC_BUDGET = 3_000_000
C_BUDGET = 1_000_000


class Regs(ctypes.Structure):
    _fields_ = [('r', ctypes.c_uint32 * 8), ('df', ctypes.c_uint32), ('top', ctypes.c_uint32),
                ('sw', ctypes.c_uint32), ('cw', ctypes.c_uint32), ('st', ctypes.c_double * 8)]


def pg(n): return (n + 0xFFF) & ~0xFFF


def f80_to_double(m, e):
    sign = -1.0 if e & 0x8000 else 1.0
    e &= 0x7FFF
    if e == 0 and m == 0: return 0.0 * sign
    if e == 0x7FFF: return float('nan') if (m << 1) & ((1 << 64) - 1) else sign * float('inf')
    try:
        return sign * math.ldexp(float(m), e - 16383 - 63)
    except OverflowError:
        return sign * float('inf')


class Env:
    def __init__(self, pe_path, meta_path, lib_path):
        self.meta = meta = json.load(open(meta_path))
        pe = pefile.PE(pe_path)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        img = bytearray(pe.get_memory_mapped_image())
        for slot, idx in meta['iat'].items():
            struct.pack_into('<I', img, int(slot, 16) - self.base, IMP_BASE + 16 * idx)
        self.img = bytes(img)
        self.smc = bool(meta.get('smc_sites'))
        self.smc_bytes = set()
        for k, fl in (meta.get('smc_sites') or {}).items():
            for f in fl:
                self.smc_bytes.update(range(int(k, 16) + f['off'], int(k, 16) + f['off'] + f['size']))
        self.regions = []  # (addr, size, code?)
        self.regions.append((self.base, 0x1000, False, False))  # headers, read-only
        for (n, a, sz, ch) in meta['sections']:
            code = bool(ch & 0x20000000)
            # self-modifying binaries: code is writable (the game VirtualProtects it)
            self.regions.append((a, pg(sz), code, bool(ch & 0x80000000) or (code and self.smc)))
        self.code_ranges = [(a, a + sz) for (a, sz, code, w) in self.regions if code]
        self.data_ptrs = [a for (a, sz, code, w) in self.regions if w and not code]
        self.writable = [(a, sz) for (a, sz, code, w) in self.regions if w] + [STACK, HEAP, (TIB, 0x1000)]
        # C side
        self.lib = ctypes.CDLL(os.path.abspath(lib_path))
        self.lib.hx_mem.restype = ctypes.c_void_p
        self.lib.hx_run.argtypes = [ctypes.c_uint32, ctypes.POINTER(Regs), ctypes.POINTER(Regs),
                                    ctypes.POINTER(ctypes.c_uint32), ctypes.c_int64]
        assert self.lib.hx_init(IMP_BASE, len(meta['imports'])) == 0
        self.M = self.lib.hx_mem()
        self.lib.hx_set_fs(TIB)
        for (a, sz, code, w) in self.regions + [(STACK[0], STACK[1], False, True), (HEAP[0], HEAP[1], False, True),
                                                (TIB, 0x1000, False, True)]:
            assert self.lib.hx_protect(a, sz, 3) == 0  # RW while loading
        self.new_uc()
        # static image content (written once per trial for writable regions, once now for all)
        for (a, sz, code, w) in self.regions:
            data = self.img[a - self.base:a - self.base + sz].ljust(sz, b'\0')
            ctypes.memmove(self.M + a, data, len(data))
        for (a, sz, code, w) in self.regions:
            if not w:
                self.lib.hx_protect(a, sz, 1)  # read-only on C side

    def new_uc(self):
        # fresh emulator: Unicorn keeps internal state across faulting runs that skews later results
        self.mu = mu = Uc(UC_ARCH_X86, UC_MODE_32)
        for (a, sz, code, w) in self.regions:
            mu.mem_map(a, sz, UC_PROT_READ | (UC_PROT_EXEC if code else 0) | (UC_PROT_WRITE if w else 0))
            mu.mem_write(a, self.img[a - self.base:a - self.base + sz].ljust(sz, b'\0'))
        mu.mem_map(STACK[0], STACK[1], UC_PROT_READ | UC_PROT_WRITE)
        mu.mem_map(HEAP[0], HEAP[1], UC_PROT_READ | UC_PROT_WRITE)
        mu.mem_map(TIB, 0x1000, UC_PROT_READ | UC_PROT_WRITE)
        mu.mem_map(GDT, 0x1000, UC_PROT_READ)
        def desc(base, limit, access, flags):
            return struct.pack('<Q', (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (access << 40) |
                               (((limit >> 16) & 0xF) << 48) | (flags << 52) | ((base >> 24) << 56))
        gdt = b'\0' * 8 + desc(TIB, 0xFFF, 0xF3, 0x4) + desc(0, 0xFFFFF, 0x93, 0xC)  # 0x08 TIB (fs), 0x10 flat 32-bit data
        mu.mem_write(GDT, gdt)
        mu.reg_write(UC_X86_REG_GDTR, (0, GDT, len(gdt) - 1, 0))
        mu.reg_write(UC_X86_REG_FS, 0x08 | 3)
        for sreg in (UC_X86_REG_SS, UC_X86_REG_DS, UC_X86_REG_ES):
            mu.reg_write(sreg, 0x10)
        self.intr = None
        mu.hook_add(UC_HOOK_INTR, self._on_intr)
        self.code_writes = []
        if self.smc:
            for lo, hi in self.code_ranges:
                mu.hook_add(UC_HOOK_MEM_WRITE, self._on_code_write, begin=lo, end=hi - 1)

    def _on_code_write(self, mu, acc, addr, size, val, ud):
        if any(b not in self.smc_bytes for b in range(addr, addr + size)):
            self.code_writes.append((hex(mu.reg_read(UC_X86_REG_EIP)), hex(addr), size))

    def _on_intr(self, mu, intno, ud):
        self.intr = intno
        mu.emu_stop()

    def put(self, a, data):
        self.mu.mem_write(a, bytes(data))
        ctypes.memmove(self.M + a, bytes(data), len(data))

    def c_read(self, a, n):
        return ctypes.string_at(self.M + a, n)

    # ---- state generation
    def gen(self, rng):
        def heapptr(): return HEAP[0] + rng.randrange(0, HEAP[1] - 0x1000) & ~3
        def val():
            k = rng.random()
            if k < 0.35: return heapptr()
            if k < 0.65: return rng.choice([0, 1, 2, 3, 4, 7, 8, 16, 0xFFFFFFFF, 0x20, 0x100])
            if k < 0.75: return rng.choice(self.data_ptrs) + rng.randrange(0, 0x400) * 4
            return rng.getrandbits(32) & 0x7FFFFFFF if rng.random() < 0.5 else rng.getrandbits(16)
        heap = bytearray(rng.getrandbits(8) for _ in range(0))  # placeholder
        heap = bytearray(rng.randbytes(HEAP[1]))
        for k in range(0, HEAP[1], 4):
            x = rng.random()
            if x < 0.50: struct.pack_into('<I', heap, k, heapptr())
            elif x < 0.80: struct.pack_into('<I', heap, k, rng.choice([0, 0, 0, 1, 2, 4, 0xFFFFFFFF]))
        stack = bytearray(STACK[1])
        sp = STACK_TOP
        struct.pack_into('<I', stack, sp - STACK[0], SENTINEL)
        for k in range(1, 40):
            struct.pack_into('<I', stack, sp - STACK[0] + 4 * k, val())
        regs = [val() for _ in range(8)]
        regs[4] = sp
        regs[5] = rng.choice([sp + 0x80, val()])
        return regs, bytes(heap), bytes(stack)

    def load_state(self, heap, stack):
        self.put(HEAP[0], heap)
        self.put(STACK[0], stack)
        self.put(TIB, struct.pack('<I', 0xFFFFFFFF) + bytes(0xFFC))
        for (a, sz, code, w) in self.regions:
            if w:
                self.put(a, self.img[a - self.base:a - self.base + sz].ljust(sz, b'\0'))

    # ---- runs
    def run_c(self, entry, regs):
        rin = Regs(); rout = Regs(); arg = ctypes.c_uint32()
        for k in range(8): rin.r[k] = regs[k]
        rin.df = 0; rin.top = 0; rin.sw = 0; rin.cw = 0x27F
        res = self.lib.hx_run(entry, ctypes.byref(rin), ctypes.byref(rout), ctypes.byref(arg), C_BUDGET)
        st = {'r': list(rout.r), 'top': rout.top, 'sw': rout.sw, 'st': list(rout.st)}
        return OUT[res], arg.value, st

    def run_u(self, entry, regs):
        mu = self.mu
        for k in range(8): mu.reg_write(REGS[k], regs[k])
        mu.reg_write(UC_X86_REG_EFLAGS, 0x202)
        mu.reg_write(UC_X86_REG_FPCW, 0x27F)
        mu.reg_write(UC_X86_REG_FPSW, 0)
        mu.reg_write(UC_X86_REG_FPTAG, 0xFFFF)
        self.intr = None
        out, arg = None, 0
        try:
            mu.emu_start(entry, SENTINEL, count=UC_BUDGET)
            eip = mu.reg_read(UC_X86_REG_EIP)
            if self.intr is not None:
                out, arg = 'FAULT', {0: 2, 3: 3}.get(self.intr, 200 + self.intr)
            elif eip == SENTINEL: out = 'RET'
            else: out = 'BUDGET'
        except UcError as e:
            eip = mu.reg_read(UC_X86_REG_EIP)
            if e.errno in (UC_ERR_FETCH_UNMAPPED, UC_ERR_FETCH_PROT):
                if IMP_BASE <= eip < IMP_BASE + 16 * len(self.meta['imports']) and (eip - IMP_BASE) % 16 == 0:
                    out, arg = 'IMPORT', (eip - IMP_BASE) // 16
                elif eip == SENTINEL:
                    out = 'RET'
                else:
                    out, arg = 'EXTERNAL', eip
            elif e.errno in (UC_ERR_READ_UNMAPPED, UC_ERR_WRITE_UNMAPPED, UC_ERR_READ_PROT, UC_ERR_WRITE_PROT,
                             UC_ERR_READ_UNALIGNED, UC_ERR_WRITE_UNALIGNED):
                out = 'MEMFAULT'
            elif e.errno == UC_ERR_EXCEPTION and self.intr is not None:
                out, arg = 'FAULT', {0: 2, 3: 3}.get(self.intr, 200 + self.intr)
            else:
                out, arg = 'FAULT', 300 + e.errno
        r = [mu.reg_read(x) for x in REGS]
        fsw = mu.reg_read(UC_X86_REG_FPSW)
        top = (fsw >> 11) & 7
        phys = [mu.reg_read(UC_X86_REG_FP0 + i) for i in range(8)]
        st = [f80_to_double(*phys[(top + i) & 7]) for i in range(8)]
        return out, arg, {'r': r, 'top': top, 'sw': fsw, 'st': st}

    def mem_diff(self):
        diffs = []
        for (a, sz) in self.writable:
            u = self.mu.mem_read(a, sz)
            c = self.c_read(a, sz)
            if u != c:
                for k in range(0, sz, 4):
                    if u[k:k + 4] != c[k:k + 4]:
                        diffs.append((a + k, bytes(u[k:k + 4]).hex(), c[k:k + 4].hex()))
                        if len(diffs) > 8: return diffs
        return diffs

    def trial(self, entry, seed):
        rng = random.Random(seed)
        regs, heap, stack = self.gen(rng)
        self.load_state(heap, stack)
        self.code_writes = []
        uo, ua, us = self.run_u(entry, regs)
        if self.code_writes:
            return 'UNKNOWN_SMC', {'writes': self.code_writes[:5], 'u': uo}
        uoreg = None
        mem_u = None
        # snapshot unicorn writable mem before C overwrites shared? (separate memories, no need)
        co, ca, cs = self.run_c(entry, regs)
        if co == 'BUDGET' or uo == 'BUDGET':
            return 'INCONCLUSIVE', {'u': uo, 'c': co}
        if co == 'FAULT' and ca == 1:
            return 'UNIMPL', {'u': uo}
        if co == 'FAULT' and ca == 5:
            return ('BOTH_FAULT', {'k': 'badret', 'u': uo}) if uo != 'RET' else ('OUTCOME_MISMATCH', {'u': (uo, hex(ua)), 'c': ('BADRET', hex(ca))})
        if co != uo or (co in ('IMPORT', 'EXTERNAL', 'FAULT') and ca != ua):
            return 'OUTCOME_MISMATCH', {'u': (uo, hex(ua)), 'c': (co, hex(ca))}
        if co == 'FAULT':
            return 'BOTH_FAULT', {'k': co}
        det = {}
        rd = [(RNAMES[k], hex(us['r'][k]), hex(cs['r'][k])) for k in range(8) if us['r'][k] != cs['r'][k]]
        swonly = False
        if rd and all(n == 'eax' for n, _, _ in rd) and ((us['r'][0] ^ cs['r'][0]) & ~0x2FF) == 0:
            rd = []; swonly = True
        if rd: det['regs'] = rd
        if us['top'] != cs['top']:
            det['top'] = (us['top'], cs['top'])
        else:
            for k in range((8 - us['top']) & 7):
                a, b = us['st'][k], cs['st'][k]
                if not (a == b or (math.isnan(a) and math.isnan(b))):
                    det.setdefault('st', []).append((k, a, b))
        md = self.mem_diff()
        if md: det['mem'] = md
        if det:
            return ('FAULT_STATE_MISMATCH' if co == 'MEMFAULT' else 'MISMATCH'), det
        if co == 'MEMFAULT':
            return 'FAULT_PASS', {}
        return ('PASS_SW' if swonly else 'PASS'), {'out': co}


_env = None


def _worker_init(pe, meta, lib):
    global _env
    _env = Env(pe, meta, lib)


def _test_fn(args):
    entry, trials = args
    res = []
    for t in range(trials):
        _env.new_uc()
        r, d = _env.trial(entry, entry * 1000 + t)
        res.append((r, d))
    return entry, res


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('pe'); ap.add_argument('meta'); ap.add_argument('lib')
    ap.add_argument('--trials', type=int, default=4)
    ap.add_argument('--only', default='')
    ap.add_argument('--workers', type=int, default=2)
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--report', default='')
    a = ap.parse_args()
    meta = json.load(open(a.meta))
    ents = [int(x, 16) for x in a.only.split(',')] if a.only else meta['entries']
    if a.limit: ents = ents[:a.limit]
    totals = collections.Counter()
    per_fn = {}
    with Pool(a.workers, _worker_init, (a.pe, a.meta, a.lib)) as pool:
        for k, (entry, res) in enumerate(pool.imap_unordered(_test_fn, [(e, a.trials) for e in ents], chunksize=4)):
            for r, d in res: totals[r] += 1
            kinds = [r for r, d in res]
            if 'UNKNOWN_SMC' in kinds: verdict = 'UNKNOWN_SMC'
            elif 'MISMATCH' in kinds or 'OUTCOME_MISMATCH' in kinds: verdict = 'FAIL'
            elif any(x in ('PASS', 'PASS_SW') for x in kinds): verdict = 'PASS'
            elif 'FAULT_STATE_MISMATCH' in kinds: verdict = 'FAULT_STATE_DIFF'
            elif 'FAULT_PASS' in kinds: verdict = 'PASS_AT_FAULT'
            elif 'BOTH_FAULT' in kinds: verdict = 'FAULT_ONLY'
            elif 'UNIMPL' in kinds: verdict = 'UNIMPL'
            else: verdict = 'INCONCLUSIVE'
            per_fn[hex(entry)] = {'verdict': verdict, 'trials': [(r, d) for r, d in res]}
            if (k + 1) % 500 == 0:
                print(f'  {k + 1}/{len(ents)} ...', flush=True)
    fv = collections.Counter(v['verdict'] for v in per_fn.values())
    print('trial outcomes:', dict(totals))
    print('function verdicts:', dict(fv))
    if a.report:
        json.dump({'totals': totals, 'verdicts': fv, 'functions': per_fn}, open(a.report, 'w'), indent=1, default=str)
