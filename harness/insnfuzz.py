#!/usr/bin/env python3
"""Per-instruction differential fuzzer.

gen: collect every distinct instruction encoding reached in the given PE files, wrap each in a
     test stub, lift the stubs to C.
run: execute each stub on many random register/flag/memory/x87 states in both the recompiled C
     and Unicorn; compare registers, defined flags, memory and x87 stack.

stub layout (at CODE + 64*k):
    push dword [FLAGSIN] ; popfd          load random input flags
    fld qword [FPIN+8*j]  (x n)          preload x87 stack (x87 tests only)
    <instruction under test>              (absolute memory operands repointed into SCR)
    pushfd ; pop dword [FLAGSOUT] ; ret
"""
import sys, os, struct, random, json, math, ctypes, argparse, collections, re
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
import lift
from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_AC_WRITE
from capstone.x86 import *
from multiprocessing import Pool
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_PROT_READ, UC_PROT_WRITE, UC_PROT_EXEC, UC_HOOK_INTR
from unicorn.x86_const import *

CODE = 0x00400000
STACK = (0x00200000, 0x10000); ESP0 = 0x0020F000
SCR = (0x30000000, 0x10000)
IO = 0x30010000           # FLAGSIN, FLAGSOUT, FPIN[8]
FLAGSIN, FLAGSOUT, FPIN = IO, IO + 4, IO + 16
SENTINEL = 0x00010000
STUB = 64
CF, PF, AF, ZF, SF, DF, OF = 1, 4, 0x10, 0x40, 0x80, 0x400, 0x800
ALLF = CF | PF | ZF | SF | OF | DF
REGS = [UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
        UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI]
RN = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']

SKIP = {'call', 'jmp', 'ret', 'retf', 'iretd', 'int', 'int1', 'int3', 'into', 'hlt', 'ud2', 'in', 'out', 'insb', 'insd',
        'insw', 'outsb', 'outsd', 'outsw', 'les', 'lds', 'lfs', 'lgs', 'lss', 'bound', 'aam', 'aad', 'daa', 'das', 'aaa',
        'aas', 'salc', 'sldt', 'arpl', 'push', 'pop', 'pushal', 'popal', 'pushfd', 'popfd', 'leave', 'enter', 'ljmp',
        'lcall', 'cli', 'sti', 'fnsave', 'frstor', 'fldenv', 'fnstenv', 'fbstp', 'fbld', 'fldcw', 'cpuid', 'rdtsc',
        'wait', 'nop', 'fnop', 'ffree'}


def norm_key(i):
    s = re.sub(r'0x[0-9a-f]+', 'N', i.op_str)
    s = re.sub(r'\b\d+\b', 'N', s)
    return i.mnemonic + ' ' + s


def collect(paths, per_key=6):
    keys = collections.defaultdict(list)
    seen = set()
    for p in paths:
        L = lift.Lifter(p, 'x')
        for e in sorted(L.entries):
            f = lift.Fn(L, e)
            f.walk()
            for va, i in f.insns.items():
                if i is None: continue
                b = bytes(i.bytes)
                if b in seen: continue
                seen.add(b)
                m = i.mnemonic.replace('lock ', '')
                if m in SKIP or m.startswith('j') or m.startswith('loop'):
                    continue
                if X86_GRP_MMX in i.groups or X86_GRP_SSE1 in i.groups or X86_GRP_SSE2 in i.groups:
                    continue
                regs_r, regs_w = i.regs_access()
                names = {i.reg_name(r) for r in list(regs_r) + list(regs_w)}
                if {'esp', 'sp', 'cs', 'ds', 'es', 'fs', 'gs', 'ss'} & {i.reg_name(r) for r in regs_w}:
                    continue
                if any(o.type == X86_OP_REG and i.reg_name(o.reg) in ('cs', 'ds', 'es', 'fs', 'gs', 'ss') for o in i.operands):
                    continue
                if i.prefix[1] in (0x64, 0x65, 0x2E, 0x36, 0x3E, 0x26) and not m.split()[-1][:4] in ('movs', 'stos', 'lods', 'scas', 'cmps'):
                    continue
                if i.mnemonic == 'fnstsw' and i.operands and i.operands[0].type == X86_OP_MEM:
                    continue
                k = norm_key(i)
                if len(keys[k]) < per_key:
                    keys[k].append(i)
    return keys


def build_stub(i):
    """returns (bytes, info) for one stub."""
    b = bytearray(i.bytes)
    info = {'asm': f'{i.mnemonic} {i.op_str}', 'm': i.mnemonic.replace('lock ', ''), 'mem': None, 'x87': 0}
    mem = [o for o in i.operands if o.type == X86_OP_MEM]
    if mem and i.mnemonic != 'lea' and not re.search(r'(movs|stos|lods|scas|cmps)[bwd]$', i.mnemonic):
        m = mem[0].mem
        if m.base == 0 and m.index == 0:
            if i.disp_size != 4: return None
            info['mem'] = {'abs': i.disp_offset}
        else:
            info['mem'] = {'base': i.reg_name(m.base) if m.base else None,
                           'index': i.reg_name(m.index) if m.index else None,
                           'scale': m.scale, 'disp': m.disp & 0xFFFFFFFF}
    if i.mnemonic.startswith('f'):
        mx = 0
        for o in i.operands:
            if o.type == X86_OP_REG and i.reg_name(o.reg).startswith('st'):
                mx = max(mx, int(i.reg_name(o.reg)[3:-1]) if '(' in i.reg_name(o.reg) else 0)
        info['x87'] = min(7, max(2, mx + 1))
    pre = bytearray(b'\xFF\x35' + struct.pack('<I', FLAGSIN) + b'\x9D')
    for j in range(info['x87']):
        pre += b'\xDD\x05' + struct.pack('<I', FPIN + 8 * j)
    info['insn_off'] = len(pre)
    post = b'\x9C\x8F\x05' + struct.pack('<I', FLAGSOUT) + b'\xC3'
    stub = pre + b + post
    if len(stub) > STUB: return None
    return stub, info


FCOMI = ('fcomi', 'fcompi', 'fucomi', 'fucompi')


def defined_flags(info, regs, cnt):
    m = info['m'].split()[-1]
    if m in ('add', 'adc', 'sub', 'sbb', 'cmp', 'neg', 'inc', 'dec', 'and', 'or', 'xor', 'test') or m[:4] in ('cmps', 'scas'):
        return ALLF
    if m in ('shl', 'sal', 'shr', 'sar'):
        c = cnt & 31
        if c == 0: return ALLF
        W = info['w'] * 8
        f = PF | ZF | SF | DF
        if c < W: f |= CF
        if c == 1: f |= OF
        return f
    if m in ('rol', 'ror', 'rcl', 'rcr'):
        c = cnt & 31
        if c == 0: return ALLF
        return (PF | ZF | SF | DF | CF) | (OF if c == 1 else 0)
    if m in ('shld', 'shrd'):
        c = cnt & 31
        if c == 0: return ALLF
        return (CF | PF | ZF | SF | DF) | (OF if c == 1 else 0)
    if m in FCOMI:
        # Intel SDM: ZF,PF,CF set from the compare, OF/SF/AF cleared. Unicorn leaves OF/SF unchanged, so only
        # ZF/PF/CF are compared against it here; OF/SF == 0 is checked against the SDM separately in run_one.
        return CF | PF | ZF | DF
    if m in ('mul', 'imul'): return CF | OF | DF
    if m in ('div', 'idiv'): return DF
    if m in ('bt', 'bts', 'btr', 'btc'): return CF | ZF | DF
    if m in ('bsf', 'bsr'): return ZF | DF
    return ALLF


def gen(paths, outdir):
    keys = collect(paths)
    stubs, infos = bytearray(), []
    for k in sorted(keys):
        for i in keys[k]:
            r = build_stub(i)
            if not r: continue
            s, info = r
            info['key'] = k
            info['w'] = max([o.size for o in i.operands] or [4])
            if info['m'].split()[-1] in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr'):
                info['w'] = i.operands[0].size
                c = i.operands[1] if len(i.operands) > 1 else None
                info['cnt'] = ('imm', c.imm) if c is not None and c.type == X86_OP_IMM else ('cl', 0) if c is not None else ('imm', 1)
            if info['m'] in ('shld', 'shrd'):
                c = i.operands[2]
                info['cnt'] = ('imm', c.imm) if c.type == X86_OP_IMM else ('cl', 0)
            info['va'] = CODE + len(stubs)
            infos.append(info)
            stubs += s.ljust(STUB, b'\xCC')
    # patch absolute memory operands to point into SCR
    rng = random.Random(1)
    for info in infos:
        if info['mem'] and 'abs' in info['mem']:
            t = SCR[0] + 0x100 + rng.randrange(0, SCR[1] - 0x400)
            off = info['va'] - CODE + info['insn_off'] + info['mem']['abs']
            struct.pack_into('<I', stubs, off, t)
    img = lift.SynthImage(CODE, bytes(stubs), [x['va'] for x in infos])
    L = lift.Lifter(None, 'fz', img=img)
    meta = L.run(outdir, per_file=400)
    open(os.path.join(outdir, 'stubs.bin'), 'wb').write(stubs)
    json.dump(infos, open(os.path.join(outdir, 'stubs.json'), 'w'))
    print(f'{len(keys)} shapes, {len(infos)} stubs; lifter unimpl: {meta["unimpl"]}')


# ------------------------------------------------------------------ run

class Regs(ctypes.Structure):
    _fields_ = [('r', ctypes.c_uint32 * 8), ('df', ctypes.c_uint32), ('top', ctypes.c_uint32),
                ('sw', ctypes.c_uint32), ('cw', ctypes.c_uint32), ('st', ctypes.c_double * 8)]


def f80(m, e):
    sign = -1.0 if e & 0x8000 else 1.0
    e &= 0x7FFF
    if e == 0 and m == 0: return 0.0 * sign
    if e == 0x7FFF: return float('nan') if (m << 1) & ((1 << 64) - 1) else sign * float('inf')
    try: return sign * math.ldexp(float(m), e - 16383 - 63)
    except OverflowError: return sign * float('inf')


class Fuzzer:
    def __init__(self, outdir, lib):
        self.stubs = open(os.path.join(outdir, 'stubs.bin'), 'rb').read()
        self.infos = json.load(open(os.path.join(outdir, 'stubs.json')))
        self.lib = ctypes.CDLL(os.path.abspath(lib))
        self.lib.hx_mem.restype = ctypes.c_void_p
        self.lib.hx_run.argtypes = [ctypes.c_uint32, ctypes.POINTER(Regs), ctypes.POINTER(Regs),
                                    ctypes.POINTER(ctypes.c_uint32), ctypes.c_int64]
        self.lib.hx_init(0x7FFE0000, 0)
        self.M = self.lib.hx_mem()
        codesz = (len(self.stubs) + 0xFFF) & ~0xFFF
        self.regions = [(CODE, codesz, True), (STACK[0], STACK[1], False), (SCR[0], SCR[1], False), (IO, 0x1000, False)]
        for a, n, code in self.regions:
            self.lib.hx_protect(a, n, 3)
        ctypes.memmove(self.M + CODE, self.stubs, len(self.stubs))
        self.lib.hx_protect(CODE, codesz, 1)
        self.scr_pool = []
        prng = random.Random(42)
        for _ in range(6):
            scr = bytearray()
            for _ in range(SCR[1] // 4):
                k = prng.random()
                if k < 0.4: scr += struct.pack('<I', prng.getrandbits(32))
                elif k < 0.7: scr += struct.pack('<f', prng.choice([0.0, 1.0, -1.0, 0.5, 3.0]) * prng.uniform(-1e3, 1e3))
                else: scr += struct.pack('<I', prng.choice([0, 1, 0xFFFFFFFF, prng.randrange(0, 256)]))
            self.scr_pool.append(bytes(scr))

    def new_uc(self):
        mu = Uc(UC_ARCH_X86, UC_MODE_32)
        for a, n, code in self.regions:
            mu.mem_map(a, n, UC_PROT_READ | (UC_PROT_EXEC if code else UC_PROT_WRITE))
        mu.mem_write(CODE, self.stubs)
        self.intr = None
        def onint(mu, n, u): self.intr = n; mu.emu_stop()
        mu.hook_add(UC_HOOK_INTR, onint)
        return mu

    def put(self, mu, a, data):
        mu.mem_write(a, data)
        ctypes.memmove(self.M + a, data, len(data))

    def state(self, rng, info):
        def rv():
            k = rng.random()
            if k < 0.25: return rng.getrandbits(32)
            if k < 0.45: return rng.choice([0, 1, 2, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF,
                                            0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFFE, 0x80000001])
            if k < 0.70: return rng.randrange(0, 64)
            if k < 0.85: return (-rng.randrange(1, 64)) & 0xFFFFFFFF
            return rng.getrandbits(8) << rng.choice([0, 8, 16, 24])
        regs = [rv() for _ in range(8)]
        regs[4] = ESP0
        m = info['m'].split()[-1]
        if re.search(r'(movs|stos|lods|scas|cmps)[bwd]$', m) or m == 'xlatb':
            regs[6] = SCR[0] + 0x2000 + rng.randrange(0, 0x400)
            regs[7] = SCR[0] + 0x8000 + rng.randrange(0, 0x400)
            regs[1] = rng.randrange(0, 48)
            if rng.random() < 0.15: regs[7] = regs[6] + rng.randrange(-8, 9)   # overlapping copies
            regs[3] = SCR[0] + 0x4000
        if m in ('div', 'idiv') and rng.random() < 0.7:
            regs[2] = rng.choice([0, 0, 1, 0xFFFFFFFF, rng.randrange(0, 8)])
        mi = info['mem']
        if mi and 'abs' not in mi and all(r in (None,) + tuple(RN) for r in (mi['base'], mi['index'])):
            t = SCR[0] + 0x100 + rng.randrange(0, SCR[1] - 0x400)
            ri = lambda n: RN.index(n) if n in RN else None
            b, x, sc, d = mi['base'], mi['index'], mi['scale'], mi['disp']
            if b == 'esp':
                if x: regs[ri(x)] = rng.randrange(0, 16)
            elif x and b == x:
                k = 1 + sc
                v = ((t - d) // k) & 0xFFFFFFFF
                regs[ri(b)] = v
            elif x and not b:
                regs[ri(x)] = ((t - d) // sc) & 0xFFFFFFFF
            else:
                xv = 0
                if x:
                    xv = rng.randrange(0, 64) if rng.random() < 0.8 else (-rng.randrange(1, 32)) & 0xFFFFFFFF
                    regs[ri(x)] = xv
                if b:
                    regs[ri(b)] = (t - d - xv * sc) & 0xFFFFFFFF
        flags = 0x202 | sum(f for f in (CF, PF, AF, ZF, SF, OF) if rng.random() < 0.5) | (DF if rng.random() < 0.15 else 0)
        scr = rng.choice(self.scr_pool)
        fpin = [rng.choice([0.0, 1.0, -2.5, 1e-3, 3.0e5, rng.uniform(-100, 100), rng.uniform(-1e6, 1e6),
                            rng.uniform(-1, 1), float(rng.randrange(-1000, 1000))]) for _ in range(8)]
        if m in FCOMI + ('fcom', 'fcomp', 'fucom', 'fucomp', 'fcompp', 'fucompp'):
            # unordered / infinite operands exercise the NaN branch of the compare flags
            fpin = [rng.choice([float('nan'), float('inf'), float('-inf')] + [x for x in fpin]) for x in range(8)]
        rc = rng.randrange(4) if m in ('fistp', 'fist', 'frndint') else 0
        cw = 0x27F | (rc << 10)
        return regs, flags, bytes(scr), fpin, cw

    def run_one(self, k, seed):
        info = self.infos[k]
        rng = random.Random(seed)
        regs, flags, scr, fpin, cw = self.state(rng, info)
        mu = self.new_uc()
        stack = bytearray(STACK[1]); struct.pack_into('<I', stack, ESP0 - STACK[0], SENTINEL)
        io = bytearray(0x1000)
        struct.pack_into('<I', io, 0, flags)
        for j, v in enumerate(fpin): struct.pack_into('<d', io, 16 + 8 * j, v)
        for a, d in ((STACK[0], bytes(stack)), (SCR[0], scr), (IO, bytes(io))):
            self.put(mu, a, d)
        # unicorn
        for j in range(8): mu.reg_write(REGS[j], regs[j])
        mu.reg_write(UC_X86_REG_FPCW, cw); mu.reg_write(UC_X86_REG_FPSW, 0); mu.reg_write(UC_X86_REG_FPTAG, 0xFFFF)
        uo = 'RET'
        try:
            mu.emu_start(info['va'], SENTINEL, count=100000)
            if self.intr is not None: uo = f'INT{self.intr}'
        except UcError as e:
            uo = 'RET' if mu.reg_read(UC_X86_REG_EIP) == SENTINEL else f'ERR{e.errno}'
        ur = [mu.reg_read(x) for x in REGS]
        fsw = mu.reg_read(UC_X86_REG_FPSW); utop = (fsw >> 11) & 7
        phys = [mu.reg_read(UC_X86_REG_FP0 + j) for j in range(8)]
        ust = [f80(*phys[(utop + j) & 7]) for j in range(8)]
        umem = {a: bytes(mu.mem_read(a, n)) for a, n, code in self.regions if not code}
        # C
        rin, rout, arg = Regs(), Regs(), ctypes.c_uint32()
        for j in range(8): rin.r[j] = regs[j]
        rin.cw = cw
        res = self.lib.hx_run(info['va'], ctypes.byref(rin), ctypes.byref(rout), ctypes.byref(arg), 1 << 20)
        co = {0: 'RET', 3: 'MEMFAULT', 4: f'FAULT{arg.value}'}.get(res, f'OUT{res}')
        if co == 'FAULT1': return 'UNIMPL', None
        if co == 'FAULT2' and uo == 'INT0': return 'PASS_DIVFAULT', None
        if co != 'RET' or uo != 'RET':
            if co in ('MEMFAULT', 'FAULT5') and uo in ('ERR6', 'ERR7', 'ERR8'):
                return 'BOTH_FAULT', None
            return 'OUTCOME', {'u': uo, 'c': co}
        det = {}
        cr = list(rout.r)
        for j in range(8):
            if ur[j] != cr[j]:
                d = ur[j] ^ cr[j]
                if j == 0 and info['m'] == 'fnstsw' and (d & ~0x2FF) == 0: continue
                if j == 0 and info['m'] == 'lahf' and (d & ~0x1000) == 0: continue   # AF (AH bit 4) not modelled: known gap
                det.setdefault('regs', []).append((RN[j], hex(regs[j]), hex(ur[j]), hex(cr[j])))
        cnt = 0
        if 'cnt' in info:
            cnt = info['cnt'][1] if info['cnt'][0] == 'imm' else regs[1] & 0xFF
        fm = defined_flags(info, regs, cnt)
        cmem = {a: ctypes.string_at(self.M + a, n) for a, n, code in self.regions if not code}
        uf = struct.unpack_from('<I', umem[IO], 4)[0]; cf = struct.unpack_from('<I', cmem[IO], 4)[0]
        if (uf ^ cf) & fm:
            det['flags'] = (hex(flags), hex(uf & fm), hex(cf & fm))
        for a in umem:
            u, c = umem[a], cmem[a]
            if a == IO: u, c = u[:4] + u[8:], c[:4] + c[8:]
            if a == STACK[0]:  # stub's own pushfd slot (flags compared separately, masked)
                o = ESP0 - 4 - STACK[0]; u = u[:o] + u[o + 4:]; c = c[:o] + c[o + 4:]
            if u != c:
                for o in range(0, len(u), 4):
                    if u[o:o + 4] != c[o:o + 4]:
                        det.setdefault('mem', []).append((hex(a + o), u[o:o + 4].hex(), c[o:o + 4].hex()))
                        if len(det['mem']) > 4: break
        if info['m'] in FCOMI and (cf & (OF | SF)):
            det['flags_sdm'] = ('OF/SF must be cleared', hex(cf & (OF | SF)))
        ctop = rout.top
        if utop != ctop:
            det['top'] = (utop, ctop)
        elif info['x87']:
            depth = (8 - utop) & 7 or 8
            for j in range(depth):
                a, b = ust[j], rout.st[j]
                if not (a == b or (math.isnan(a) and math.isnan(b))):
                    det.setdefault('st', []).append((j, a, b))
        if det:
            det['in'] = {'regs': [hex(x) for x in regs], 'flags': hex(flags), 'fpin': fpin[:info['x87']], 'cw': hex(cw)}
            return 'MISMATCH', det
        return 'PASS', None


_fz = None
def _init(outdir, lib):
    global _fz
    _fz = Fuzzer(outdir, lib)


def _job(args):
    k, n = args
    res = collections.Counter(); ex = []
    for t in range(n):
        r, d = _fz.run_one(k, k * 7919 + t)
        res[r] += 1
        if d is not None and len(ex) < 2: ex.append((r, d))
    return k, dict(res), ex


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd')
    g = sub.add_parser('gen'); g.add_argument('outdir'); g.add_argument('pe', nargs='+')
    r = sub.add_parser('run'); r.add_argument('outdir'); r.add_argument('lib')
    r.add_argument('--trials', type=int, default=24); r.add_argument('--workers', type=int, default=2)
    r.add_argument('--report', default=''); r.add_argument('--only', default='')
    a = ap.parse_args()
    if a.cmd == 'gen':
        gen(a.pe, a.outdir)
    else:
        infos = json.load(open(os.path.join(a.outdir, 'stubs.json')))
        ks = range(len(infos)) if not a.only else [int(x) for x in a.only.split(',')]
        tot = collections.Counter(); bykey = collections.defaultdict(collections.Counter); fails = {}
        with Pool(a.workers, _init, (a.outdir, a.lib)) as pool:
            for k, res, ex in pool.imap_unordered(_job, [(k, a.trials) for k in ks], chunksize=8):
                tot.update(res)
                key = infos[k]['key']
                bykey[key].update(res)
                if ex and any(x[0] in ('MISMATCH', 'OUTCOME') for x in ex):
                    fails[k] = {'asm': infos[k]['asm'], 'key': key, 'res': res, 'ex': ex}
        badkeys = sorted(k for k, c in bykey.items() if c['MISMATCH'] or c['OUTCOME'])
        print('trials:', dict(tot))
        print(f'shapes: {len(bykey)}, failing shapes: {len(badkeys)}')
        for k in badkeys[:60]: print('  ', k, dict(bykey[k]))
        if a.report:
            json.dump({'tot': tot, 'bykey': bykey, 'fails': fails}, open(a.report, 'w'), indent=1, default=str)
