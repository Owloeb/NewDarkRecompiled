#!/usr/bin/env python3
"""darkrecomp: static recompiler for 32-bit x86 PE (Dark Engine) -> portable C.

usage: lift.py <pe> <prefix> <outdir> [--funcs-per-file N]
"""
import sys, os, re, struct, collections, argparse, json
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import *

R16 = {'ax': 'eax', 'cx': 'ecx', 'dx': 'edx', 'bx': 'ebx', 'sp': 'esp', 'bp': 'ebp', 'si': 'esi', 'di': 'edi'}
R8L = {'al': 'eax', 'cl': 'ecx', 'dl': 'edx', 'bl': 'ebx'}
R8H = {'ah': 'eax', 'ch': 'ecx', 'dh': 'edx', 'bh': 'ebx'}
R32 = {'eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi'}
UT = {1: 'uint8_t', 2: 'uint16_t', 4: 'uint32_t', 8: 'uint64_t'}
ST_ = {1: 'int8_t', 2: 'int16_t', 4: 'int32_t', 8: 'int64_t'}
SB = {1: '0x80u', 2: '0x8000u', 4: '0x80000000u'}
BITS = {1: 8, 2: 16, 4: 32}
CC = {'o': 'OF', 'no': '!OF', 'b': 'CF', 'ae': '!CF', 'e': 'ZF', 'ne': '!ZF', 'be': '(CF||ZF)',
      'a': '(!CF&&!ZF)', 's': 'SF', 'ns': '!SF', 'p': 'PF', 'np': '!PF', 'l': '(SF!=OF)',
      'ge': '(SF==OF)', 'le': '(ZF||SF!=OF)', 'g': '(!ZF&&SF==OF)'}
CC_ALIAS = {'c': 'b', 'nae': 'b', 'nb': 'ae', 'nc': 'ae', 'z': 'e', 'nz': 'ne', 'na': 'be', 'nbe': 'a',
            'pe': 'p', 'po': 'np', 'nge': 'l', 'nl': 'ge', 'ng': 'le', 'nle': 'g'}


import capstone.x86_const as _xc
FLN = ['CF', 'ZF', 'SF', 'OF', 'PF']
_FB = {'CF': 1, 'ZF': 2, 'SF': 4, 'OF': 8, 'PF': 16}
ALLFL = 31


def flag_usedef(i):
    """(use, def) masks over FLN for one instruction, as the emitted C treats them."""
    m = i.mnemonic.replace('lock ', '')
    if m in ('fcomi', 'fcompi', 'fucomi', 'fucompi'): return 0, ALLFL
    if X86_GRP_FPU in i.groups or m.startswith('f'):
        return 0, 0
    if m in ('pushfd', 'pushf', 'lahf'): return ALLFL, 0
    if m in ('popfd', 'popf'): return 0, ALLFL
    if m == 'sahf': return 0, _FB['CF'] | _FB['PF'] | _FB['ZF'] | _FB['SF']
    e = i.eflags
    use = dfn = 0
    for f in FLN:
        if e & getattr(_xc, 'X86_EFLAGS_TEST_' + f): use |= _FB[f]
        for k in ('MODIFY_', 'RESET_', 'SET_', 'UNDEFINED_'):
            if e & getattr(_xc, 'X86_EFLAGS_' + k + f): dfn |= _FB[f]
    base = m.split()[-1]
    # count may be zero at runtime -> flags pass through: don't treat as a kill
    if base in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr', 'shld', 'shrd'):
        last = i.operands[-1] if i.operands else None
        if last is not None and not (last.type == X86_OP_IMM and (last.imm & 31)):
            dfn = 0
    if m.startswith(('rep ', 'repe ', 'repne ', 'repz ', 'repnz ')):
        dfn = 0
    if base in ('rcl', 'rcr'): use |= _FB['CF'] | _FB['OF']   # rcr reads OF-ish via CF chain; be conservative
    # emitted code for xor r,r sets all; jcc/setcc reads exactly what cond() uses (capstone agrees)
    return use, dfn


def flagset(mask):
    return [f for f in FLN if mask & _FB[f]]


def cond(cc):
    cc = CC_ALIAS.get(cc, cc)
    return CC[cc]


class Unsupported(Exception):
    pass


class Image:
    def __init__(self, path, rebase=None):
        self.pe = pe = pefile.PE(path)
        if rebase is not None and rebase != pe.OPTIONAL_HEADER.ImageBase:
            pe.relocate_image(rebase)   # lift as if loaded at `rebase` (the host maps the DLL there itself)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.img = bytes(pe.get_memory_mapped_image())
        self.sections = []
        for s in pe.sections:
            a = self.base + s.VirtualAddress
            n = max(s.Misc_VirtualSize, s.SizeOfRawData)
            self.sections.append((s.Name.rstrip(b'\0').decode(), a, n, s.Characteristics))
        self.code = [(a, a + n) for (_, a, n, c) in self.sections if c & 0x20000000]
        self.entry = self.base + pe.OPTIONAL_HEADER.AddressOfEntryPoint
        self.exports = {}
        if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
            for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
                self.exports[self.base + e.address] = e.name.decode() if e.name else f'ord{e.ordinal}'
        self.imports = []        # [(dll, name)]
        self.iat = {}            # slot va -> import index
        for d in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
            for i in d.imports:
                nm = i.name.decode() if i.name else f'ord{i.ordinal}'
                self.iat[i.address] = len(self.imports)
                self.imports.append((d.dll.decode(), nm))
        self.jt_ranges = set()   # (lo, hi) byte ranges of jump tables found so far
        self.iat_indirect = False   # set for DLLs whose imports Windows/the host binds (--iat-indirect)
        self.relocs = []
        if hasattr(pe, 'DIRECTORY_ENTRY_BASERELOC'):
            for blk in pe.DIRECTORY_ENTRY_BASERELOC:
                for e in blk.entries:
                    if e.type == 3:
                        self.relocs.append(e.rva + self.base)
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = True
        self._cache = {}

    def is_code(self, va):
        return any(a <= va < b for a, b in self.code)

    def rd32(self, va):
        return struct.unpack_from('<I', self.img, va - self.base)[0]

    def decode(self, va):
        if va in self._cache:
            return self._cache[va]
        off = va - self.base
        i = next(self.md.disasm(self.img[off:off + 16], va), None) if self.is_code(va) else None
        if i is not None and i.mnemonic == 'repz ret':
            i = _AsRet(i)
        if len(self._cache) > 250000:   # bounded: full-detail capstone objects are large
            self._cache.clear()
        self._cache[va] = i
        return i


class _AsRet:
    """'repz ret' (MSVC branch-predictor idiom) is a plain ret."""
    def __init__(self, i): self._i = i
    def __getattr__(self, k): return getattr(self._i, k)
    mnemonic = 'ret'


# ------------------------------------------------------------------ analysis

def jump_table(img, i):
    op = i.operands[0]
    if op.type == X86_OP_MEM and op.mem.base == 0 and op.mem.index != 0 and op.mem.scale == 4 \
            and op.mem.segment == 0:
        t = op.mem.disp & 0xFFFFFFFF
        ents = []
        k, gap = 0, 0
        while k < 2048:
            try:
                v = img.rd32(t + 4 * k)
            except struct.error:
                break
            k += 1
            if not img.is_code(v):
                gap += 1
                if gap >= 2: break      # tolerate one unused slot (e.g. MSVC memcpy LeadUpVec[0])
                continue
            gap = 0
            ents.append(v)
        len_fwd = k - gap   # slots up to the last code pointer
        # negative indices (e.g. MSVC memcpy: jmp [ecx*4 + vec + 16] with ecx in -4..-1)
        k = 1
        while k <= 16:
            try:
                v = img.rd32(t - 4 * k)
            except struct.error:
                break
            if not img.is_code(v):
                break
            ents.append(v)
            k += 1
        img.jt_ranges.add((t - 4 * (k - 1), t + 4 * len_fwd))   # the table's own bytes: data, never a function
        return t, ents
    return None


def successors(img, i, entries, fentry):
    """returns (intra_targets, falls_through)"""
    m = i.mnemonic
    nxt = i.address + i.size
    if m in ('ret', 'retf', 'hlt', 'int3', 'ud2', 'iretd'):
        return [], False
    if m == 'jmp':
        op = i.operands[0]
        if op.type == X86_OP_IMM:
            t = op.imm & 0xFFFFFFFF
            if t in entries and t != fentry:
                return [], False
            return [t], False
        jt = jump_table(img, i)
        if jt:
            return list(dict.fromkeys(jt[1])), False
        return [], False
    if m.startswith('j') or m.startswith('loop'):
        op = i.operands[0]
        t = op.imm & 0xFFFFFFFF
        if t in entries and t != fentry:
            return [], True
        return [t], True
    return [], True


def discover(img):
    """find function entries."""
    entries = set([img.entry]) | set(img.exports)
    jt_targets = set()
    # relocated pointers into code: function pointers, vtables, callbacks
    reloc_code_ptrs = set()
    if img.relocs:
        for r in img.relocs:
            v = img.rd32(r)
            if img.is_code(v):
                reloc_code_ptrs.add((r, v))
    else:
        for (_, a, n, c) in img.sections:
            if c & 0x20000000:
                continue   # code: switch tables are found via jump_table(), callbacks via immediates
            for k in range(0, n - 3, 4):
                try:
                    v = img.rd32(a + k)
                except struct.error:
                    break
                if img.is_code(v):
                    reloc_code_ptrs.add((a + k, v))
    # traverse everything reachable to find call targets and jump tables
    seen = set()
    work = list(entries)
    calls = set()
    while True:
        while work:
            va = work.pop()
            while va not in seen:
                i = img.decode(va)
                if i is None:
                    break
                seen.add(va)
                m = i.mnemonic
                if m == 'call' and i.operands[0].type == X86_OP_IMM:
                    t = i.operands[0].imm & 0xFFFFFFFF
                    if img.is_code(t):
                        calls.add(t)
                        work.append(t)
                if m == 'jmp' or m.startswith('j') or m.startswith('loop'):
                    if i.operands[0].type == X86_OP_IMM:
                        work.append(i.operands[0].imm & 0xFFFFFFFF)
                    elif m == 'jmp':
                        jt = jump_table(img, i)
                        if jt:
                            jt_targets.update(jt[1])
                            work.extend(jt[1])
                if m in ('ret', 'retf', 'jmp', 'hlt', 'int3', 'iretd', 'ud2'):
                    break
                va += i.size
        # Seeds are taken in priority order so that phantom entries can't pre-empt real code:
        #  1. relocated pointers located in data (function pointers, vtables, callbacks), immediates in reached code
        #  2. only when nothing else is left: relocated pointers located in code. A relocation that sits inside a
        #     reached instruction is just an operand (an immediate, or a memory displacement such as the self-patching
        #     mappers' `mov [field], reg`, whose target is a patch field in the middle of another instruction), so
        #     those are skipped; what remains are tables embedded in code.
        # (jump-table entries are intra-function labels and never seed.)
        new = [v for (r, v) in reloc_code_ptrs if not img.is_code(r) and v not in seen and v not in jt_targets]
        # immediates in reached code that point into code (push offset callback, etc.)
        for va in list(seen):
            i = img.decode(va)
            if i.mnemonic in ('call', 'jmp') or i.mnemonic.startswith('j') or i.mnemonic.startswith('loop'):
                continue
            for o in i.operands:
                if o.type == X86_OP_IMM and img.is_code(o.imm & 0xFFFFFFFF):
                    v = o.imm & 0xFFFFFFFF
                    if v not in seen and v not in jt_targets:
                        new.append(v)
        if not [v for v in set(new) if img.decode(v) is not None and v not in entries]:
            inop = set()
            for va in seen:
                i = img.decode(va)
                if i is not None:
                    inop.update(range(va, va + i.size))
            new = [v for (r, v) in reloc_code_ptrs if img.is_code(r) and r not in inop and v not in seen and v not in jt_targets]
        new = [v for v in set(new) if img.decode(v) is not None]
        if not new:
            break
        work.extend(new)
        entries.update(new)
    entries |= calls
    # pointers (data or imm) that land on reached instruction starts and aren't jump-table targets
    for (r, v) in reloc_code_ptrs:
        if v in seen and v not in jt_targets:
            entries.add(v)
    entries = {e for e in entries if img.decode(e) is not None}
    # a jump table embedded in code can look like a function when a stray pointer lands on it; it is data
    in_table = [e for e in entries if any(lo <= e < hi for lo, hi in img.jt_ranges)]
    if in_table: print(f'discover: dropped {len(in_table)} "functions" that are jump-table data: ' + ' '.join(f'{e:x}' for e in sorted(in_table)[:12]))
    entries -= set(in_table)
    return entries, jt_targets


# ------------------------------------------------------------------ emission

# --cache-regs: guest registers and the memory base live in locals of each function (see runtime/rt_fast.h).
# Applied as a rewrite of the emitted C, so the default output is untouched.
_CR_REG = re.compile(r'c->(eax|ecx|edx|ebx|esp|ebp|esi|edi)\b')
_CR_MEM = re.compile(r'\b(RD8|RD16|RD32|RD64|WR8|WR16|WR32|WR64|LDF32|LDF64|LDF80|STF32|STF64|STF80)\(')
_CR_RET = re.compile(r'\breturn;')


_CR_NAMES = ('eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi')
_CR_WR = re.compile(r'\br_(eax|ecx|edx|ebx|esp|ebp|esi|edi)\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)')
_CR_INC = re.compile(r'(?:\+\+|--)\s*r_(eax|ecx|edx|ebx|esp|ebp|esi|edi)\b|\br_(eax|ecx|edx|ebx|esp|ebp|esi|edi)\s*(?:\+\+|--)')


def cache_regs_writes(line):
    """the cached registers a rewritten line assigns"""
    w = set(_CR_WR.findall(line))
    for a, b in _CR_INC.findall(line): w.add(a or b)
    if 'MPUSH32(' in line or 'MPOP32(' in line or 'MCALLPUSH(' in line: w.add('esp')
    return w


def cache_regs_spill(regs):
    if not regs: return '((void)0)'
    return '((void)(' + ', '.join(f'c->{r} = r_{r}' for r in _CR_NAMES if r in regs) + '))'


def cache_regs_reload(regs):
    if not regs: return '((void)0)'
    return '((void)(' + ', '.join(f'r_{r} = c->{r}' for r in _CR_NAMES if r in regs) + '))'


_CR_TOK = re.compile(r'\br_(eax|ecx|edx|ebx|esp|ebp|esi|edi)\b')


def cache_regs(line, prefix):
    l = _CR_REG.sub(r'r_\1', line)
    l = _CR_MEM.sub(r'M\1(', l)
    l = l.replace('CALLPUSH(c, ', 'MCALLPUSH(').replace('PUSH32(c, ', 'MPUSH32(').replace('POP32(c)', 'MPOP32()') \
         .replace('RETCHK(c)', 'MRETCHK()').replace('BUDGET()', 'MBUDGET()')
    # control leaves for the host or another guest function: registers go back to the CPU struct, and come back after
    l = re.sub(r'\b((?:rt_call_import|rt_cpuid|%s_call|%s_[0-9a-f]{8})\(c(?:, [^;]*)?\);)' % (prefix, prefix),
               r'{ SPILL_; \1 RELOAD_; }', l)
    l = re.sub(r'\bRT_SETJMP\(c\);', '{ SPILL_; RT_SETJMP(c); RELOAD_; }', l)
    l = re.sub(r'\b((?:rt_longjmp\(c\)|rt_fault\(c, [^;]*\));)', r'{ SPILLALL_; \1 }', l)
    l = _CR_RET.sub('{ SPILL_; return; }', l)
    return l


class Fn:
    def __init__(self, lifter, entry):
        self.L = lifter
        self.img = lifter.img
        self.entry = entry
        self.insns = {}
        self.labels = set()
        self.unimpl = collections.Counter()

    def walk(self):
        work = [self.entry]
        while work:
            va = work.pop()
            if va in self.insns:
                continue
            i = self.img.decode(va)
            self.insns[va] = i
            if i is None:
                continue
            tg, ft = successors(self.img, i, self.L.entries, self.entry)
            for t in list(tg) + [t for t in self.L.smc_branch.get(va, ()) if not (t in self.L.entries and t != self.entry)]:
                self.labels.add(t)
                work.append(t)
            if ft:
                work.append(va + i.size)
        self.labels.add(self.entry)

    # ---- operand helpers
    def reg_rd(self, n):
        if n in R32: return f'c->{n}'
        if n in R16: return f'(uint16_t)c->{R16[n]}'
        if n in R8L: return f'(uint8_t)c->{R8L[n]}'
        if n in R8H: return f'(uint8_t)(c->{R8H[n]}>>8)'
        raise Unsupported(f'reg {n}')

    def reg_wr(self, n, v):
        if n in R32: return f'c->{n} = (uint32_t)({v});'
        if n in R16: r = R16[n]; return f'c->{r} = (c->{r} & 0xFFFF0000u) | (uint16_t)({v});'
        if n in R8L: r = R8L[n]; return f'c->{r} = (c->{r} & 0xFFFFFF00u) | (uint8_t)({v});'
        if n in R8H: r = R8H[n]; return f'c->{r} = (c->{r} & 0xFFFF00FFu) | ((uint32_t)(uint8_t)({v}) << 8);'
        raise Unsupported(f'reg {n}')

    def ea(self, i, op):
        m = op.mem
        if m.segment not in (0, X86_REG_DS, X86_REG_SS, X86_REG_ES, X86_REG_CS, X86_REG_FS):
            raise Unsupported('segment')
        parts = []
        if m.segment == X86_REG_FS:
            parts.append('c->fs_base')
        for r in (m.base, m.index):
            if r and i.reg_name(r) not in R32:
                raise Unsupported('16-bit addressing')
        if m.base:
            parts.append(f'c->{i.reg_name(m.base)}')
        if m.index:
            ix = f'c->{i.reg_name(m.index)}'
            parts.append(ix if m.scale == 1 else f'{ix}*{m.scale}u')
        pf = self.patched(i, 'disp')
        if pf:
            parts.append(f'RD{8 * pf["size"]}(0x{i.address + pf["off"]:x}u) /* SMC disp */' if pf['size'] == 4 else
                         f'(uint32_t)(int{8 * pf["size"]}_t)RD{8 * pf["size"]}(0x{i.address + pf["off"]:x}u) /* SMC disp */')
        elif m.disp or not parts:
            parts.append(f'0x{m.disp & 0xFFFFFFFF:x}u')
        return '(uint32_t)(' + ' + '.join(parts) + ')'

    def rd(self, i, op, size=None):
        size = size or op.size
        if op.type == X86_OP_REG: return self.reg_rd(i.reg_name(op.reg))
        if op.type == X86_OP_IMM:
            pf = self.patched(i, 'imm')
            if pf:
                return f'(({UT[size]})(int{8 * pf["size"]}_t)RD{8 * pf["size"]}(0x{i.address + pf["off"]:x}u) /* SMC imm */)'
            return f'(({UT[size]})0x{op.imm & ((1 << (8 * size)) - 1):x}u)'
        if op.type == X86_OP_MEM: return f'RD{8 * size}(ea)'
        raise Unsupported('operand')

    def patched(self, i, kind):
        for f in self.L.smc_sites.get(i.address, ()):
            if f['kind'] == kind:
                return f
        return None

    def dyn_branch(self, i, cond=None):
        """branch whose displacement is patched at runtime: dispatch over every value the code writes."""
        pf = self.patched(i, 'imm')
        nxt = i.address + i.size
        L = [f'uint32_t t = 0x{nxt:x}u + (uint32_t)(int{8 * pf["size"]}_t)RD{8 * pf["size"]}(0x{i.address + pf["off"]:x}u); /* SMC branch */']
        body = ['switch (t) {']
        for t in sorted(self.L.smc_branch.get(i.address, ())):
            body.append(f'    case 0x{t:x}u: {self.target(t)}')
        body += [f'    default: rt_fault(c, t, RT_FAULT_BADJUMP);', '}']
        if cond:
            return L + [f'if ({cond}) {{'] + ['    ' + b for b in body] + ['}']
        return L + body

    def wr(self, i, op, v):
        if op.type == X86_OP_REG: return self.reg_wr(i.reg_name(op.reg), v)
        if op.type == X86_OP_MEM: return f'WR{8 * op.size}(ea, ({UT[op.size]})({v}));'
        raise Unsupported('operand write')

    def fl_store(self, mask):
        return ''.join(f'c->{f.lower()} = {f}; ' for f in flagset(mask))

    def fl_load(self, mask):
        return ''.join(f'{f} = c->{f.lower()}; ' for f in flagset(mask))

    def target(self, t, tail_cond=None):
        """jump to address t: goto label or tail call."""
        if t in self.L.entries and t != self.entry:
            s = f'{{ {self.fl_store(self.L.fl_in.get(t, 0))}{self.L.fname(t)}(c); return; }}'
        else:
            s = f'goto L_{t:x};'
        return f'if ({tail_cond}) {s}' if tail_cond else s

    # ---- main
    def emit(self):
        out = [f'void {self.L.fname(self.entry)}(CPU *c) {{',
               '    uint8_t CF = 0, ZF = 0, SF = 0, OF = 0, PF = 0;',
               '    (void)CF; (void)ZF; (void)SF; (void)OF; (void)PF;',
               f'    {self.fl_load(self.L.fl_in.get(self.entry, 0))}BUDGET(); goto L_{self.entry:x};']
        addrs = sorted(self.insns)
        span = {}
        for k, va in enumerate(addrs):
            i = self.insns[va]
            span[va] = len(out)
            out.append(f'L_{va:x}: ;' if (va in self.labels or k == 0 or addrs[k - 1] + (self.insns[addrs[k - 1]].size if self.insns[addrs[k - 1]] else 0) != va or True) else '')
            if va in self.labels and va != self.entry:
                out.append('    BUDGET();')
            if i is None:
                out.append(f'    rt_fault(c, 0x{va:x}u, RT_FAULT_UNIMPL); /* undecodable */')
                self.unimpl['<undecodable>'] += 1
                continue
            out.append(f'    /* {va:08x}: {i.mnemonic} {i.op_str} */ TRACE(0x{va:x}u);')
            try:
                body = self.insn(i)
            except Unsupported as e:
                self.unimpl[i.mnemonic] += 1
                body = [f'rt_fault(c, 0x{va:x}u, RT_FAULT_UNIMPL); /* {e} */']
            out.extend('    ' + b for b in body)
            # explicit fall-through if next emitted insn isn't adjacent
            tg, ft = successors(self.img, i, self.L.entries, self.entry)
            if ft:
                nxt = va + i.size
                if k + 1 >= len(addrs) or addrs[k + 1] != nxt:
                    out.append(f'    goto L_{nxt:x};')
        out.append('}')
        # every label referenced must exist: labels are all insns here, so fine
        if self.L.cache_regs:
            out = self.cache_regs_rewrite(out, addrs, span)
        return '\n'.join(l for l in out if l != '')

    def cache_regs_rewrite(self, out, addrs, span):
        """--cache-regs: rewrite the function, then decide per instruction which registers its calls/returns must store:
        those it may have changed since the last point where locals and the CPU struct agreed (function entry, or after a
        direct call, which stores and reloads unconditionally). A forward may-analysis over the instruction graph."""
        new = [out[0], '    RT_LOCALS;'] + [cache_regs(l, self.L.prefix) for l in out[1:]]
        owner = [None] * len(new)
        bounds = [span[va] for va in addrs] + [len(out) - 1]
        for k, va in enumerate(addrs):
            for j in range(bounds[k], bounds[k + 1]): owner[j + 1] = va      # +1: RT_LOCALS inserted
        writes = collections.defaultdict(set)
        for j, l in enumerate(new):
            if owner[j] is not None: writes[owner[j]] |= cache_regs_writes(l)
        succ, callee = {}, {}
        ALL = frozenset(_CR_NAMES)
        for va in addrs:
            i = self.insns[va]
            if i is None: succ[va] = []; continue
            tg, ft = successors(self.img, i, self.L.entries, self.entry)
            s = [t for t in list(tg) + list(self.L.smc_branch.get(va, ())) if t in self.insns]
            if ft and va + i.size in self.insns: s.append(va + i.size)
            succ[va] = s
            if i.mnemonic == 'call':       # what the called function may read and write (ALL when unknown)
                op = i.operands[0]; t = op.imm & 0xFFFFFFFF if op.type == X86_OP_IMM else None
                callee[va] = self.L.reg_sum.get(t, (ALL, ALL)) if t is not None and t not in self.L.setjmp_fns \
                    and t not in self.L.longjmp_fns else (ALL, ALL)
        din = {va: set() for va in addrs}
        work = list(addrs)
        while work:
            va = work.pop()
            if va in callee:               # after the call, registers it may touch are in sync again
                R, W = callee[va]; dout = (din[va] | writes[va]) - R - W
            else:
                dout = din[va] | writes[va]
            for s in succ[va]:
                if not dout <= din[s]:
                    din[s] |= dout; work.append(s)
        for j, l in enumerate(new):
            va = owner[j]
            if va in callee:
                R, W = callee[va]
                l = l.replace('SPILL_;', cache_regs_spill((din[va] | writes[va]) & (R | W)) + ';')
                l = l.replace('RELOAD_;', cache_regs_reload(W) + ';')
            if 'SPILL_;' in l:
                regs = set(_CR_NAMES) if va is None else din[va] | writes[va]
                l = l.replace('SPILL_;', cache_regs_spill(regs) + ';')
            new[j] = l
        return new

    def insn(self, i):
        m = i.mnemonic
        ops = i.operands
        va = i.address
        nxt = va + i.size
        if m.startswith('lock '):
            m = m[5:]
        if i.prefix[0] in (0xF3, 0xF2) and not (m.startswith('rep')):
            pass
        pre = []
        mem = [o for o in ops if o.type == X86_OP_MEM]
        is_string = any(m.endswith(s) or m.split()[-1] == s for s in
                        ('movsb', 'movsw', 'movsd', 'stosb', 'stosw', 'stosd', 'lodsb', 'lodsw', 'lodsd',
                         'scasb', 'scasw', 'scasd', 'cmpsb', 'cmpsw', 'cmpsd'))
        if is_string:
            return self.string(i, m)
        if mem and m not in ('lea', 'pop') and not m.startswith('f') and m != 'nop':
            pre = [f'uint32_t ea = {self.ea(i, mem[0])};']
        fl = getattr(self, 'i_' + m, None)
        if m.startswith('f') or m in ('wait',):
            return ['{'] + ['    ' + x for x in self.x87(i)] + ['}']
        if fl is None:
            if m.startswith('set') and (m[3:] in CC or m[3:] in CC_ALIAS):
                fl = self.i_setcc
            elif m.startswith('j') and (m[1:] in CC or m[1:] in CC_ALIAS):
                fl = self.i_jcc
            else:
                raise Unsupported(m)
        body = fl(i, m, ops, nxt)
        return ['{'] + ['    ' + x for x in pre + body] + ['}']

    # ---------------- integer ops
    def _szp(self, r):
        return f'ZF = ({r}) == 0; SF = (({r}) & SB_) != 0; PF = PAR({r});'

    def _arith(self, i, ops, kind):
        d, s = ops
        n = d.size
        T = UT[n]
        sb = SB[n]
        a = self.rd(i, d)
        b = self.rd(i, s, n)
        L = [f'{T} a = {a}, b = {b}, r;']
        if kind == 'add':
            L += ['r = a + b; CF = r < a;', f'OF = (((a ^ r) & (b ^ r)) & {sb}) != 0;']
        elif kind == 'adc':
            L += ['r = a + b + CF; CF = CF ? (r <= a) : (r < a);', f'OF = (((a ^ r) & (b ^ r)) & {sb}) != 0;']
        elif kind in ('sub', 'cmp'):
            L += ['r = a - b; CF = a < b;', f'OF = (((a ^ b) & (a ^ r)) & {sb}) != 0;']
        elif kind == 'sbb':
            L += ['r = a - b - CF; CF = CF ? (a <= b) : (a < b);', f'OF = (((a ^ b) & (a ^ r)) & {sb}) != 0;']
        elif kind in ('and', 'test'):
            L += ['r = a & b; CF = 0; OF = 0;']
        elif kind == 'or':
            L += ['r = a | b; CF = 0; OF = 0;']
        elif kind == 'xor':
            L += ['r = a ^ b; CF = 0; OF = 0;']
        L += [self._szp('r').replace('SB_', sb)]
        if kind not in ('cmp', 'test'):
            L.append(self.wr(i, d, 'r'))
        return L

    def i_add(self, i, m, o, n): return self._arith(i, o, 'add')
    def i_adc(self, i, m, o, n): return self._arith(i, o, 'adc')
    def i_sub(self, i, m, o, n): return self._arith(i, o, 'sub')
    def i_sbb(self, i, m, o, n): return self._arith(i, o, 'sbb')
    def i_cmp(self, i, m, o, n): return self._arith(i, o, 'cmp')
    def i_and(self, i, m, o, n): return self._arith(i, o, 'and')
    def i_or(self, i, m, o, n): return self._arith(i, o, 'or')
    def i_xor(self, i, m, o, n):
        if o[0].type == X86_OP_REG and o[1].type == X86_OP_REG and o[0].reg == o[1].reg:
            return [self.wr(i, o[0], '0'), 'CF = 0; OF = 0; ZF = 1; SF = 0; PF = 1;']
        return self._arith(i, o, 'xor')
    def i_test(self, i, m, o, n): return self._arith(i, o, 'test')

    def i_inc(self, i, m, o, n):
        d = o[0]; T = UT[d.size]; sb = SB[d.size]
        return [f'{T} r = ({T})({self.rd(i, d)} + 1u); OF = r == {sb};', self._szp('r').replace('SB_', sb), self.wr(i, d, 'r')]

    def i_dec(self, i, m, o, n):
        d = o[0]; T = UT[d.size]; sb = SB[d.size]
        return [f'{T} r = ({T})({self.rd(i, d)} - 1u); OF = r == ({T})({sb} - 1u);', self._szp('r').replace('SB_', sb), self.wr(i, d, 'r')]

    def i_neg(self, i, m, o, n):
        d = o[0]; T = UT[d.size]; sb = SB[d.size]
        return [f'{T} a = {self.rd(i, d)}, r = ({T})(0u - a); CF = a != 0; OF = a == {sb};',
                self._szp('r').replace('SB_', sb), self.wr(i, d, 'r')]

    def i_not(self, i, m, o, n):
        d = o[0]; T = UT[d.size]
        return [self.wr(i, d, f'({T})~{self.rd(i, d)}')]

    def i_mov(self, i, m, o, n):
        return [self.wr(i, o[0], self.rd(i, o[1], o[0].size))]

    def i_movzx(self, i, m, o, n):
        return [self.wr(i, o[0], f'(uint32_t){self.rd(i, o[1])}')]

    def i_movsx(self, i, m, o, n):
        return [self.wr(i, o[0], f'(uint32_t)(int32_t)({ST_[o[1].size]}){self.rd(i, o[1])}')]

    def i_lea(self, i, m, o, n):
        return [self.wr(i, o[0], self.ea(i, o[1]))]

    def i_xchg(self, i, m, o, n):
        a, b = o
        if a.type == X86_OP_REG and b.type == X86_OP_REG and a.reg == b.reg:
            return []
        T = UT[a.size]
        return [f'{T} t1 = {self.rd(i, a)}, t2 = {self.rd(i, b)};', self.wr(i, a, 't2'), self.wr(i, b, 't1')]

    def i_bswap(self, i, m, o, n):
        return [self.wr(i, o[0], f'__builtin_bswap32({self.rd(i, o[0])})')]

    def i_push(self, i, m, o, n):
        d = o[0]
        if d.size == 2:
            return [f'uint16_t v = {self.rd(i, d)}; c->esp -= 2; WR16(c->esp, v);']
        return [f'uint32_t v = {self.rd(i, d, 4)}; PUSH32(c, v);']

    def i_pop(self, i, m, o, n):
        d = o[0]
        if d.size == 2:
            if d.type == X86_OP_MEM: raise Unsupported('pop m16')
            return [f'uint16_t v = RD16(c->esp); c->esp += 2;', self.wr(i, d, 'v')]
        if d.type == X86_OP_MEM:
            return ['uint32_t v = POP32(c);', f'uint32_t ea = {self.ea(i, d)};', 'WR32(ea, v);']
        return ['uint32_t v = POP32(c);', self.wr(i, d, 'v')]

    def i_pushal(self, i, m, o, n):
        return ['uint32_t t = c->esp;'] + [f'PUSH32(c, {x});' for x in
                ('c->eax', 'c->ecx', 'c->edx', 'c->ebx', 't', 'c->ebp', 'c->esi', 'c->edi')]

    def i_popal(self, i, m, o, n):
        return ['c->edi = POP32(c); c->esi = POP32(c); c->ebp = POP32(c); c->esp += 4;',
                'c->ebx = POP32(c); c->edx = POP32(c); c->ecx = POP32(c); c->eax = POP32(c);']

    def i_pushfd(self, i, m, o, n):
        return ['PUSH32(c, EFL_PACK(CF, PF, ZF, SF, OF, c->df));']

    def i_popfd(self, i, m, o, n):
        return ['uint32_t v = POP32(c); CF = v & 1; PF = (v >> 2) & 1; ZF = (v >> 6) & 1; SF = (v >> 7) & 1;',
                'c->df = (v >> 10) & 1; OF = (v >> 11) & 1;']

    def i_lahf(self, i, m, o, n):
        return [self.reg_wr('ah', '(SF << 7) | (ZF << 6) | (PF << 2) | 2 | CF')]

    def i_sahf(self, i, m, o, n):
        return ['uint8_t h = (uint8_t)(c->eax >> 8); CF = h & 1; PF = (h >> 2) & 1; ZF = (h >> 6) & 1; SF = (h >> 7) & 1;']

    def i_leave(self, i, m, o, n):
        return ['c->esp = c->ebp; c->ebp = POP32(c);']

    def i_enter(self, i, m, o, n):
        if (o[1].imm & 0x1F) != 0: raise Unsupported('enter nesting')
        return ['PUSH32(c, c->ebp);', 'uint32_t f = c->esp;', f'c->esp -= 0x{o[0].imm & 0xFFFF:x}u;', 'c->ebp = f;']

    def i_cdq(self, i, m, o, n): return ['c->edx = (uint32_t)((int32_t)c->eax >> 31);']
    def i_cwde(self, i, m, o, n): return ['c->eax = (uint32_t)(int32_t)(int16_t)c->eax;']
    def i_cbw(self, i, m, o, n): return [self.reg_wr('ax', '(uint16_t)(int16_t)(int8_t)c->eax')]
    def i_cwd(self, i, m, o, n): return [self.reg_wr('dx', '(uint16_t)((int16_t)c->eax >> 15)')]
    def i_cld(self, i, m, o, n): return ['c->df = 0;']
    def i_std(self, i, m, o, n): return ['c->df = 1;']
    def i_clc(self, i, m, o, n): return ['CF = 0;']
    def i_stc(self, i, m, o, n): return ['CF = 1;']
    def i_cmc(self, i, m, o, n): return ['CF = !CF;']
    def i_nop(self, i, m, o, n): return []
    def i_xlatb(self, i, m, o, n): return [self.reg_wr('al', 'RD8(c->ebx + (uint8_t)c->eax)')]
    def i_int3(self, i, m, o, n): return [f'rt_fault(c, 0x{i.address:x}u, RT_FAULT_TRAP);']

    def i_setcc(self, i, m, o, n):
        return [self.wr(i, o[0], f'({cond(m[3:])}) ? 1 : 0')]

    # shifts / rotates
    def _shift(self, i, o, kind):
        d = o[0]; T = UT[d.size]; W = BITS[d.size]; sb = SB[d.size]
        cnt = self.rd(i, o[1], 1) if len(o) > 1 else '1'
        L = [f'uint32_t n = (uint32_t)({cnt}) & 31u;', 'if (n) {', f'    {T} a = {self.rd(i, d)}, r;']
        if kind == 'shl':
            L += [f'    r = ({T})((uint32_t)a << n); CF = (uint8_t)((((uint64_t)a << n) >> {W}) & 1);',
                  f'    OF = (((r & {sb}) != 0) ^ CF);']
        elif kind == 'shr':
            L += [f'    r = ({T})((uint32_t)a >> n); CF = (uint8_t)(((uint64_t)a >> (n - 1)) & 1);',
                  f'    OF = (a & {sb}) != 0;']
        elif kind == 'sar':
            L += [f'    int32_t sa = ({ST_[d.size]})a; r = ({T})(sa >> n); CF = (uint8_t)((sa >> (n - 1)) & 1); OF = 0;']
        if kind in ('shl', 'shr', 'sar'):
            L += ['    ' + self._szp('r').replace('SB_', sb)]
        elif kind == 'rol':
            L += [f'    uint32_t k = n % {W}u; r = k ? ({T})((a << k) | (a >> ({W}u - k))) : a;',
                  f'    CF = r & 1; OF = ((r & {sb}) != 0) ^ CF;']
        elif kind == 'ror':
            L += [f'    uint32_t k = n % {W}u; r = k ? ({T})((a >> k) | (a << ({W}u - k))) : a;',
                  f'    CF = (r & {sb}) != 0; OF = CF ^ ((r >> {W - 2}) & 1);']
        elif kind == 'rcl':
            L += [f'    uint32_t k = n % {W + 1}u; r = a;',
                  f'    while (k--) {{ uint8_t hi = (r & {sb}) != 0; r = ({T})((r << 1) | CF); CF = hi; }}',
                  f'    OF = ((r & {sb}) != 0) ^ CF;']
        elif kind == 'rcr':
            L += [f'    uint32_t k = n % {W + 1}u; r = a; OF = ((r & {sb}) != 0) ^ CF;',
                  f'    while (k--) {{ uint8_t lo = r & 1; r = ({T})((r >> 1) | ((uint32_t)CF << {W - 1})); CF = lo; }}']
        L += ['    ' + self.wr(i, d, 'r'), '}']
        return L

    def i_shl(self, i, m, o, n): return self._shift(i, o, 'shl')
    def i_sal(self, i, m, o, n): return self._shift(i, o, 'shl')
    def i_shr(self, i, m, o, n): return self._shift(i, o, 'shr')
    def i_sar(self, i, m, o, n): return self._shift(i, o, 'sar')
    def i_rol(self, i, m, o, n): return self._shift(i, o, 'rol')
    def i_ror(self, i, m, o, n): return self._shift(i, o, 'ror')
    def i_rcl(self, i, m, o, n): return self._shift(i, o, 'rcl')
    def i_rcr(self, i, m, o, n): return self._shift(i, o, 'rcr')

    def i_shld(self, i, m, o, n):
        if o[0].size != 4: raise Unsupported('shld16')
        return [f'uint32_t n = (uint32_t)({self.rd(i, o[2], 1)}) & 31u;', 'if (n) {',
                f'    uint32_t a = {self.rd(i, o[0])}, b = {self.rd(i, o[1])};',
                '    uint32_t r = (a << n) | (b >> (32 - n)); CF = (a >> (32 - n)) & 1; OF = ((r ^ a) >> 31) & 1;',
                '    ' + self._szp('r').replace('SB_', '0x80000000u'), '    ' + self.wr(i, o[0], 'r'), '}']

    def i_shrd(self, i, m, o, n):
        if o[0].size != 4: raise Unsupported('shrd16')
        return [f'uint32_t n = (uint32_t)({self.rd(i, o[2], 1)}) & 31u;', 'if (n) {',
                f'    uint32_t a = {self.rd(i, o[0])}, b = {self.rd(i, o[1])};',
                '    uint32_t r = (a >> n) | (b << (32 - n)); CF = (a >> (n - 1)) & 1; OF = ((r ^ a) >> 31) & 1;',
                '    ' + self._szp('r').replace('SB_', '0x80000000u'), '    ' + self.wr(i, o[0], 'r'), '}']

    def _bt(self, i, o, kind):
        d, s = o
        if d.size != 4: raise Unsupported('bt16')
        if d.type == X86_OP_MEM and s.type == X86_OP_REG:
            L = [f'int32_t off = (int32_t){self.rd(i, s)}; uint32_t ea2 = ea + (uint32_t)((off >> 5) * 4); uint32_t bit = (uint32_t)off & 31u;',
                 'uint32_t v = RD32(ea2);']
            wb = 'WR32(ea2, v);'
        else:
            L = [f'uint32_t bit = (uint32_t)({self.rd(i, s, 4)}) & 31u; uint32_t v = {self.rd(i, d)};']
            wb = self.wr(i, d, 'v')
        L.append('CF = (v >> bit) & 1;')
        if kind == 'bts': L += ['v |= 1u << bit;', wb]
        if kind == 'btr': L += ['v &= ~(1u << bit);', wb]
        if kind == 'btc': L += ['v ^= 1u << bit;', wb]
        return L

    def i_bt(self, i, m, o, n): return self._bt(i, o, 'bt')
    def i_bts(self, i, m, o, n): return self._bt(i, o, 'bts')
    def i_btr(self, i, m, o, n): return self._bt(i, o, 'btr')
    def i_btc(self, i, m, o, n): return self._bt(i, o, 'btc')

    def i_mul(self, i, m, o, n):
        s = o[0]; sz = s.size; v = self.rd(i, s)
        if sz == 1:
            return [f'uint16_t p = (uint16_t)((uint8_t)c->eax * (uint32_t){v});', self.reg_wr('ax', 'p'), 'CF = OF = (p >> 8) != 0;']
        if sz == 2:
            return [f'uint32_t p = (uint32_t)(uint16_t)c->eax * (uint32_t){v};', self.reg_wr('ax', 'p'),
                    self.reg_wr('dx', 'p >> 16'), 'CF = OF = (p >> 16) != 0;']
        return [f'uint64_t p = (uint64_t)c->eax * (uint64_t){v};', 'c->eax = (uint32_t)p; c->edx = (uint32_t)(p >> 32);',
                'CF = OF = c->edx != 0;']

    def i_imul(self, i, m, o, n):
        if len(o) == 1:
            s = o[0]; sz = s.size; v = self.rd(i, s)
            if sz == 1:
                return [f'int16_t p = (int16_t)((int8_t)c->eax * (int8_t){v});', self.reg_wr('ax', '(uint16_t)p'),
                        'CF = OF = p != (int8_t)p;']
            if sz == 2:
                return [f'int32_t p = (int32_t)(int16_t)c->eax * (int16_t){v};', self.reg_wr('ax', '(uint16_t)p'),
                        self.reg_wr('dx', '(uint16_t)(p >> 16)'), 'CF = OF = p != (int16_t)p;']
            return [f'int64_t p = (int64_t)(int32_t)c->eax * (int32_t){v};', 'c->eax = (uint32_t)p; c->edx = (uint32_t)((uint64_t)p >> 32);',
                    'CF = OF = p != (int32_t)p;']
        d = o[0]; sz = d.size; S = ST_[sz]
        a = self.rd(i, o[0]) if len(o) == 2 else self.rd(i, o[1], sz)
        b = self.rd(i, o[1], sz) if len(o) == 2 else self.rd(i, o[2], sz)
        return [f'int64_t p = (int64_t)({S}){a} * ({S}){b};', f'CF = OF = p != ({S})p;', self.wr(i, d, f'({UT[sz]})p')]

    def i_div(self, i, m, o, n):
        s = o[0]; sz = s.size; v = self.rd(i, s); flt = f'rt_fault(c, 0x{i.address:x}u, RT_FAULT_DIVIDE);'
        if sz == 1:
            return [f'uint32_t d = {v}; if (!d) {flt}', 'uint32_t nn = (uint16_t)c->eax, q = nn / d;',
                    f'if (q > 0xFF) {flt}', self.reg_wr('al', 'q'), self.reg_wr('ah', 'nn % d')]
        if sz == 2:
            return [f'uint32_t d = {v}; if (!d) {flt}', 'uint32_t nn = ((c->edx & 0xFFFF) << 16) | (c->eax & 0xFFFF), q = nn / d;',
                    f'if (q > 0xFFFF) {flt}', self.reg_wr('ax', 'q'), self.reg_wr('dx', 'nn % d')]
        return [f'uint64_t d = {v}; if (!d) {flt}', 'uint64_t nn = ((uint64_t)c->edx << 32) | c->eax, q = nn / d;',
                f'if (q > 0xFFFFFFFFull) {flt}', 'c->eax = (uint32_t)q; c->edx = (uint32_t)(nn % d);']

    def i_idiv(self, i, m, o, n):
        s = o[0]; sz = s.size; v = self.rd(i, s); flt = f'rt_fault(c, 0x{i.address:x}u, RT_FAULT_DIVIDE);'
        if sz == 1:
            return [f'int32_t d = (int8_t){v}; if (!d) {flt}', 'int32_t nn = (int16_t)c->eax, q = nn / d;',
                    f'if (q > 127 || q < -128) {flt}', self.reg_wr('al', 'q'), self.reg_wr('ah', 'nn % d')]
        if sz == 2:
            return [f'int32_t d = (int16_t){v}; if (!d) {flt}', 'int32_t nn = (int32_t)(((c->edx & 0xFFFF) << 16) | (c->eax & 0xFFFF));',
                    f'if (d == -1 && nn == INT32_MIN) {flt}', 'int32_t q = nn / d;',
                    f'if (q > 32767 || q < -32768) {flt}', self.reg_wr('ax', 'q'), self.reg_wr('dx', 'nn % d')]
        return [f'int64_t d = (int32_t){v}; if (!d) {flt}', 'int64_t nn = (int64_t)(((uint64_t)c->edx << 32) | c->eax);',
                f'if (d == -1 && nn == INT64_MIN) {flt}', 'int64_t q = nn / d;',
                f'if (q > INT32_MAX || q < INT32_MIN) {flt}', 'c->eax = (uint32_t)q; c->edx = (uint32_t)(nn % d);']

    # ---------------- control flow
    def i_jmp(self, i, m, o, n):
        op = o[0]
        if op.type == X86_OP_IMM:
            if self.patched(i, 'imm'): return self.dyn_branch(i)
            return [self.target(op.imm & 0xFFFFFFFF)]
        if op.type == X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 and (op.mem.disp & 0xFFFFFFFF) in self.img.iat and not self.img.iat_indirect:
            return [f'rt_call_import(c, {self.img.iat[op.mem.disp & 0xFFFFFFFF]}); return; /* {self.L.impname(op.mem.disp & 0xFFFFFFFF)} */']
        jt = jump_table(self.img, i)
        if jt:
            L = ['uint32_t t = RD32(ea);', 'switch (t) {']
            for t in dict.fromkeys(jt[1]):
                L.append(f'    case 0x{t:x}u: goto L_{t:x};')
            L += [f'    default: {self.fl_store(self.L.fl_ind_in)}{self.L.prefix}_call(c, t); return;', '}']
            return L
        return [f'uint32_t t = {self.rd(i, op, 4)};', f'{self.fl_store(self.L.fl_ind_in)}{self.L.prefix}_call(c, t); return;']

    def i_jcc(self, i, m, o, n):
        if self.patched(i, 'imm'): return self.dyn_branch(i, cond(m[1:]))
        return [self.target(o[0].imm & 0xFFFFFFFF, cond(m[1:]))]

    def i_jecxz(self, i, m, o, n): return [self.target(o[0].imm & 0xFFFFFFFF, 'c->ecx == 0')]
    def i_loop(self, i, m, o, n): return ['c->ecx--;', self.target(o[0].imm & 0xFFFFFFFF, 'c->ecx != 0')]
    def i_loope(self, i, m, o, n): return ['c->ecx--;', self.target(o[0].imm & 0xFFFFFFFF, 'c->ecx != 0 && ZF')]
    def i_loopne(self, i, m, o, n): return ['c->ecx--;', self.target(o[0].imm & 0xFFFFFFFF, 'c->ecx != 0 && !ZF')]

    def i_call(self, i, m, o, n):
        op = o[0]
        if op.type == X86_OP_IMM:
            t = op.imm & 0xFFFFFFFF
            if t not in self.L.entries: raise Unsupported(f'call to non-entry {t:x}')
            if t in self.L.setjmp_fns: return [f'CALLPUSH(c, 0x{n:x}u); RT_SETJMP(c); /* setjmp */']
            if t in self.L.longjmp_fns: return [f'CALLPUSH(c, 0x{n:x}u); rt_longjmp(c); return; /* longjmp */']
            return [f'CALLPUSH(c, 0x{n:x}u); {self.fl_store(self.L.fl_in.get(t, 0))}{self.L.fname(t)}(c); {self.fl_load(self.L.fl_out.get(t, 0))}']
        if op.type == X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 and (op.mem.disp & 0xFFFFFFFF) in self.img.iat:
            s = op.mem.disp & 0xFFFFFFFF
            kind = SJ_IMPORTS.get(self.img.imports[self.img.iat[s]][1])
            if kind == 'set': return [f'CALLPUSH(c, 0x{n:x}u); RT_SETJMP(c); /* {self.L.impname(s)} */']
            if kind == 'long': return [f'CALLPUSH(c, 0x{n:x}u); rt_longjmp(c); return; /* {self.L.impname(s)} */']
            if not self.img.iat_indirect:
                return [f'CALLPUSH(c, 0x{n:x}u); rt_call_import(c, {self.img.iat[s]}); /* {self.L.impname(s)} */']
        return [f'uint32_t t = {self.rd(i, op, 4)};',
                f'CALLPUSH(c, 0x{n:x}u); {self.fl_store(self.L.fl_ind_in)}{self.L.prefix}_call(c, t); {self.fl_load(self.L.fl_ind_out)}']

    def i_cpuid(self, i, m, o, n):
        return ['rt_cpuid(c);']

    def i_ret(self, i, m, o, n):
        k = 4 + (o[0].imm if o else 0)
        return [f'RETCHK(c); c->esp += {k}u; {self.fl_store(self.L.fl_out.get(self.entry, 0))}return;']

    # ---------------- string ops
    def string(self, i, m):
        parts = m.split()
        rep = parts[0] if len(parts) == 2 else None
        op = parts[-1]
        if any(o.type == X86_OP_REG and i.reg_name(o.reg).startswith('xmm') for o in i.operands):
            raise Unsupported('sse movsd')
        if i.prefix[1] or (i.operands and any(o.type == X86_OP_MEM and o.mem.segment not in (0, X86_REG_ES, X86_REG_DS) for o in i.operands)):
            raise Unsupported('segment override string')
        sz = {'b': 1, 'w': 2, 'd': 4}[op[-1]]
        base = op[:-1]
        T = UT[sz]; B = 8 * sz
        d = f'(c->df ? (uint32_t)-{sz} : {sz}u)'
        if base == 'movs': body = [f'WR{B}(c->edi, RD{B}(c->esi)); c->esi += {d}; c->edi += {d};']
        elif base == 'stos': body = [f'WR{B}(c->edi, ({T})c->eax); c->edi += {d};']
        elif base == 'lods': body = [self.reg_wr({1: 'al', 2: 'ax', 4: 'eax'}[sz], f'RD{B}(c->esi)'), f'c->esi += {d};']
        elif base in ('scas', 'cmps'):
            a, b = (f'({T})c->eax', f'RD{B}(c->edi)') if base == 'scas' else (f'RD{B}(c->esi)', f'RD{B}(c->edi)')
            body = [f'{{ {T} a = {a}, b = {b}, r = ({T})(a - b); CF = a < b; OF = (((a ^ b) & (a ^ r)) & {SB[sz]}) != 0;',
                    '  ' + self._szp('r').replace('SB_', SB[sz]) + ' }']
            body.append(f'c->edi += {d};' if base == 'scas' else f'c->esi += {d}; c->edi += {d};')
        else:
            raise Unsupported(m)
        if rep is None:
            return ['{'] + ['    ' + b for b in body] + ['}']
        brk = ''
        if base in ('scas', 'cmps'):
            brk = ' if (!ZF) break;' if rep in ('repe', 'repz', 'rep') else ' if (ZF) break;'
        # fast path for common forward rep movs/stos (non-overlapping semantics preserved by byte order)
        return ['while (c->ecx) {'] + ['    ' + b for b in body] + [f'    c->ecx--;{brk}', '}']

    # ---------------- x87
    def x87(self, i):
        b = bytes(i.bytes)
        k = 0
        if i.mnemonic in ('wait', 'fwait'):
            return []
        while k < len(b) - 1 and b[k] in (0x66, 0x67, 0x9B, 0xF2, 0xF3, 0x2E, 0x3E, 0x26, 0x36, 0x64, 0x65, 0xF0):
            if b[k] in (0x64, 0x65): raise Unsupported('fs x87')
            k += 1
        if i.mnemonic == 'wait' or (len(b) == k + 1 and b[k] == 0x9B) or b[k] == 0x9B:
            return []
        op = b[k]
        modrm = b[k + 1]
        reg = (modrm >> 3) & 7
        mod = modrm >> 6
        rm = modrm & 7
        mem = [o for o in i.operands if o.type == X86_OP_MEM]
        AR = {0: '+', 1: '*', 4: '-', 5: 'r-', 6: '/', 7: 'r/'}

        def arith(dst, src, r):
            o = AR[r]
            if o.startswith('r'):
                return f'{dst} = {src} {o[1]} {dst};'
            return f'{dst} = {dst} {o} {src};'

        if mod != 3:
            L = [f'uint32_t ea = {self.ea(i, mem[0])};']
            if op in (0xD8, 0xDC, 0xDA, 0xDE):
                ld = {0xD8: 'LDF32(ea)', 0xDC: 'LDF64(ea)', 0xDA: '(double)(int32_t)RD32(ea)', 0xDE: '(double)(int16_t)RD16(ea)'}[op]
                L.append(f'double m = {ld};')
                if reg in (2, 3):
                    L.append('FCOM(c, ST(0), m);')
                    if reg == 3: L.append('FPOP(c);')
                else:
                    L.append(arith('ST(0)', 'm', reg))
                return L
            if op == 0xD9:
                if reg == 0: return L + ['FPUSH(c, LDF32(ea));']
                if reg == 2: return L + ['STF32(ea, ST(0));']
                if reg == 3: return L + ['STF32(ea, ST(0)); FPOP(c);']
                if reg == 4: return L + ['c->cw = RD16(ea); { uint16_t sw_ = RD16(ea + 4); c->sw = sw_ & ~0x3800; c->top = (sw_ >> 11) & 7; }']                          # fldenv (28-byte env)
                if reg == 5: return L + ['c->cw = RD16(ea);']
                if reg == 6: return L + ['WR32(ea, 0xFFFF0000u | c->cw); WR32(ea + 4, 0xFFFF0000u | FSW(c)); WR32(ea + 8, 0xFFFF0000u); WR32(ea + 12, 0); WR32(ea + 16, 0); WR32(ea + 20, 0); WR32(ea + 24, 0); c->cw |= 0x3F;']   # fnstenv (masks exceptions, like the CPU)
                if reg == 7: return L + ['WR16(ea, c->cw);']
            if op == 0xDD:
                if reg == 0: return L + ['FPUSH(c, LDF64(ea));']
                if reg == 2: return L + ['STF64(ea, ST(0));']
                if reg == 3: return L + ['STF64(ea, ST(0)); FPOP(c);']
                if reg == 4: return L + ['c->cw = RD16(ea); { uint16_t sw_ = RD16(ea + 4); c->sw = sw_ & ~0x3800; c->top = (sw_ >> 11) & 7; } for (int r_ = 0; r_ < 8; r_++) ST(r_) = LDF80(ea + 28 + 10 * r_);']   # frstor
                if reg == 6: return L + ['WR32(ea, 0xFFFF0000u | c->cw); WR32(ea + 4, 0xFFFF0000u | FSW(c)); WR32(ea + 8, 0xFFFF0000u); WR32(ea + 12, 0); WR32(ea + 16, 0); WR32(ea + 20, 0); WR32(ea + 24, 0); for (int r_ = 0; r_ < 8; r_++) STF80(ea + 28 + 10 * r_, ST(r_)); c->cw = 0x37F; c->sw = 0; c->top = 0;']   # fnsave
                if reg == 7: return L + ['WR16(ea, FSW(c));']
            if op == 0xDB:
                if reg == 0: return L + ['FPUSH(c, (double)(int32_t)RD32(ea));']
                if reg == 2: return L + ['WR32(ea, F2I32(c, ST(0)));']
                if reg == 3: return L + ['WR32(ea, F2I32(c, ST(0))); FPOP(c);']
                if reg == 5: return L + ['FPUSH(c, LDF80(ea));']
                if reg == 7: return L + ['STF80(ea, ST(0)); FPOP(c);']
            if op == 0xDF:
                if reg == 0: return L + ['FPUSH(c, (double)(int16_t)RD16(ea));']
                if reg == 2: return L + ['WR16(ea, F2I16(c, ST(0)));']
                if reg == 3: return L + ['WR16(ea, F2I16(c, ST(0))); FPOP(c);']
                if reg == 5: return L + ['FPUSH(c, (double)(int64_t)RD64(ea));']
                if reg == 7: return L + ['WR64(ea, F2I64(c, ST(0))); FPOP(c);']
            raise Unsupported(f'x87 mem {op:02x}/{reg}')
        # register forms
        si = f'ST({rm})'
        if (op in (0xDB, 0xDF)) and 0xE8 <= modrm <= 0xF7:
            pop = ' FPOP(c);' if op == 0xDF else ''
            return [f'{{ double a_ = ST(0), b_ = {si}; if (isnan(a_) || isnan(b_)) {{ ZF = PF = CF = 1; }} '
                    f'else {{ PF = 0; CF = a_ < b_; ZF = a_ == b_; }} OF = 0; SF = 0; }}{pop}']
        if op == 0xD8:
            if reg in (2, 3):
                return ['FCOM(c, ST(0), %s);' % si] + (['FPOP(c);'] if reg == 3 else [])
            return [arith('ST(0)', si, reg)]
        if op in (0xDC, 0xDE):
            if op == 0xDE and modrm == 0xD9:
                return ['FCOM(c, ST(0), ST(1)); FPOP(c); FPOP(c);']
            if reg in (2, 3):
                return ['FCOM(c, ST(0), %s);' % si] + (['FPOP(c);'] if reg == 3 else [])
            # DC/DE: dst = st(i); note reversed sense of sub/div vs D8
            rev = {0: 0, 1: 1, 4: 5, 5: 4, 6: 7, 7: 6}[reg]
            o = AR[rev]
            if o.startswith('r'):
                s = f'{si} = ST(0) {o[1]} {si};'
            else:
                s = f'{si} = {si} {o} ST(0);'
            return [s] + (['FPOP(c);'] if op == 0xDE else [])
        if op == 0xD9:
            R = 'if (fabs(ST(0)) >= 9223372036854775808.0) c->sw |= 0x400; else '
            if reg == 0: return [f'double v = {si}; FPUSH(c, v);']
            if reg == 1: return [f'double t = ST(0); ST(0) = {si}; {si} = t;']
            simple = {
                0xD0: [], 0xE0: ['ST(0) = -ST(0);'], 0xE1: ['ST(0) = fabs(ST(0));'],
                0xE4: ['FCOM(c, ST(0), 0.0);'], 0xE5: ['FXAM(c);'],
                0xE8: ['FPUSH(c, 1.0);'], 0xE9: ['FPUSH(c, 3.321928094887362347870);'],
                0xEA: ['FPUSH(c, 1.442695040888963407360);'], 0xEB: ['FPUSH(c, 3.141592653589793238463);'],
                0xEC: ['FPUSH(c, 0.301029995663981195214);'], 0xED: ['FPUSH(c, 0.693147180559945309417);'],
                0xEE: ['FPUSH(c, 0.0);'],
                0xF0: ['ST(0) = exp2(ST(0)) - 1.0;'],
                0xF1: ['ST(1) = ST(1) * log2(ST(0)); FPOP(c);'],
                0xF2: [R + '{ ST(0) = tan(ST(0)); FPUSH(c, 1.0); c->sw &= ~0x400; }'],
                0xF3: ['ST(1) = atan2(ST(1), ST(0)); FPOP(c);'],
                # fprem/fprem1 reduce completely (C2 = 0) and report the low 3 quotient bits in C0/C3/C1 like the CPU
                0xF8: ['{ double a_ = ST(0), b_ = ST(1), r_ = fmod(a_, b_); unsigned q_ = isfinite(a_) && isfinite(b_) && b_ != 0 ? (unsigned)(uint64_t)fabs(trunc((a_ - r_) / b_)) : 0; ST(0) = r_; c->sw = (uint16_t)((c->sw & ~0x4700) | ((q_ & 1) ? 0x4000 : 0) | ((q_ & 2) ? 0x200 : 0) | ((q_ & 4) ? 0x100 : 0)); }'],
                0xF5: ['{ int qi_ = 0; double r_ = remquo(ST(0), ST(1), &qi_); unsigned q_ = (unsigned)(qi_ < 0 ? -qi_ : qi_); ST(0) = r_; c->sw = (uint16_t)((c->sw & ~0x4700) | ((q_ & 1) ? 0x4000 : 0) | ((q_ & 2) ? 0x200 : 0) | ((q_ & 4) ? 0x100 : 0)); }'],
                0xFA: ['ST(0) = sqrt(ST(0));'],
                0xFB: [R + '{ double v = ST(0); ST(0) = sin(v); FPUSH(c, cos(v)); c->sw &= ~0x400; }'],
                0xFC: ['ST(0) = FROUND(c, ST(0));'],
                0xFD: ['ST(0) = ldexp(ST(0), (int)trunc(ST(1)));'],
                0xFE: [R + '{ ST(0) = sin(ST(0)); c->sw &= ~0x400; }'],
                0xFF: [R + '{ ST(0) = cos(ST(0)); c->sw &= ~0x400; }'],
                0xF6: ['c->top = (c->top - 1) & 7;'], 0xF7: ['c->top = (c->top + 1) & 7;'],
            }
            if modrm in simple: return simple[modrm]
        if op == 0xDD:
            if reg == 0: return []  # ffree
            if reg == 2: return [f'{si} = ST(0);']
            if reg == 3: return [f'{si} = ST(0); FPOP(c);']
            if reg == 4: return [f'FCOM(c, ST(0), {si});']
            if reg == 5: return [f'FCOM(c, ST(0), {si}); FPOP(c);']
        if op == 0xDA and modrm == 0xE9:
            return ['FCOM(c, ST(0), ST(1)); FPOP(c); FPOP(c);']
        if op == 0xDB and modrm == 0xE2:
            return ['c->sw &= 0x7F00;']
        if op == 0xDB and modrm == 0xE3:
            return ['c->cw = 0x37F; c->sw = 0; c->top = 0;']
        if op == 0xDF and modrm == 0xE0:
            return [self.reg_wr('ax', 'FSW(c)')]
        raise Unsupported(f'x87 {op:02x} {modrm:02x}')


class SynthImage(Image):
    """in-memory code blob (for instruction fuzzing): bytes at base, entries = given addresses."""
    def __init__(self, base, blob, entries):
        self.pe = None
        self.base = base
        self.img = bytes(blob)
        n = (len(blob) + 0xFFF) & ~0xFFF
        self.sections = [('.text', base, n, 0x60000020)]
        self.code = [(base, base + len(blob))]
        self.entry = entries[0]
        self.exports = {e: f'snip{k}' for k, e in enumerate(entries)}
        self.imports, self.iat, self.relocs = [], {}, []
        self.jt_ranges = set()
        self.iat_indirect = False
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = True
        self._cache = {}


# MSVC CRT setjmp/longjmp, statically linked into a module: recognised by their code so calls to them can be translated
# (setjmp must take its snapshot in the caller's frame; see RT_SETJMP in runtime/rt.h).
SJ_SETJMP3 = bytes.fromhex('8b542404892a895a04897a0889720c896210')   # mov edx,[esp+4]; mov [edx],ebp; ... mov [edx+10h],esp
SJ_LONGJMP = bytes.fromhex('558bec83ec508b5c2458')
SJ_IMPORTS = {'_setjmp3': 'set', '_setjmp': 'set', 'setjmp': 'set', 'longjmp': 'long'}

def find_sjlj(img, entries):
    sj, lj = set(), set()
    for lo, hi in img.code:
        blob = img.img[lo - img.base:hi - img.base]
        for pat, out in ((SJ_SETJMP3, sj), (SJ_LONGJMP, lj)):
            k = blob.find(pat)
            while k >= 0:
                if lo + k in entries: out.add(lo + k)
                k = blob.find(pat, k + 1)
    for e in entries:                          # import thunks: jmp [IAT setjmp/longjmp]
        o = e - img.base
        if 0 <= o < len(img.img) - 6 and img.img[o:o + 2] == b'\xff\x25':
            slot = int.from_bytes(img.img[o + 2:o + 6], 'little')
            if slot in img.iat:
                kind = SJ_IMPORTS.get(img.imports[img.iat[slot]][1])
                if kind == 'set': sj.add(e)
                elif kind == 'long': lj.add(e)
    if sj or lj: print(f'setjmp/longjmp: {len(sj)} setjmp, {len(lj)} longjmp function(s) recognised')
    return sj, lj


class Lifter:
    def __init__(self, path, prefix, img=None):
        self.img = img or Image(path)
        self.prefix = prefix
        self.entries, self.jt_targets = discover(self.img)
        self.setjmp_fns, self.longjmp_fns = find_sjlj(self.img, self.entries)
        self.smc_sites = {}     # insn va -> [ {kind, off, size, target} ]
        self.smc_branch = {}    # branch insn va -> set(targets)
        self.smc_problems = []
        self.fl_in, self.fl_out = {}, {}      # per function: flags read before written / flags exported at ret
        self.fl_ind_in = self.fl_ind_out = 0  # same, for indirect calls (union over address-taken functions)
        self.cache_regs = False               # --cache-regs (runtime/rt_fast.h)
        self.reg_sum = {}                     # --cache-regs: function -> (registers it may read, may write)

    def smc_analyze(self, max_iter=4):
        """find absolute writes into code; map each to the instruction field it patches. Iterates because
        patched branches can make new code reachable."""
        img = self.img
        for it in range(max_iter):
            starts = set()
            for e in self.entries:
                f = Fn(self, e); f.walk()
                starts.update(va for va, i in f.insns.items() if i is not None)
            D = img.decode
            def owners(P):
                return [t for t in range(P - 15, P + 1) if t in starts and D(t) and t + D(t).size > P]
            ft = set()
            call_targets = set()
            writes = []
            for va in starts:
                i = D(va)
                ft.add(va + i.size)
                if i.mnemonic == 'call' and i.operands and i.operands[0].type == X86_OP_IMM:
                    call_targets.add(i.operands[0].imm & 0xFFFFFFFF)
                if not i.operands or i.mnemonic in ('cmp', 'test', 'push', 'call', 'jmp') or i.mnemonic.startswith('f') \
                        or i.mnemonic.startswith('j'):
                    continue
                o = i.operands[0]
                if o.type == X86_OP_MEM and o.mem.base == 0 and o.mem.index == 0 and img.is_code(o.mem.disp & 0xFFFFFFFF):
                    writes.append((va, o.mem.disp & 0xFFFFFFFF, o.size))
            order = sorted(starts)
            pos = {va: k for k, va in enumerate(order)}
            insns = {}
            for (wva, P, sz) in writes:          # keep only what the patch mapping needs
                insns[wva] = D(wva)
                for t in owners(P): insns[t] = D(t)
                for k in range(max(0, pos[wva] - 8), pos[wva]): insns[order[k]] = D(order[k])
            owner = {P: set(owners(P)) for (_, P, _) in writes}
            sites, branch, problems = collections.defaultdict(list), collections.defaultdict(set), []
            for (wva, P, sz) in writes:
                hits = []
                for t in owner.get(P, ()):
                    i = insns[t]
                    for kind, off, fsz in (('imm', i.imm_offset, i.imm_size), ('disp', i.disp_offset, i.disp_size)):
                        if fsz and t + off == P and fsz == sz:
                            hits.append((t, kind, off, fsz))
                if len(hits) > 1:
                    hits = [h for h in hits if h[0] in ft] or hits
                if len(hits) > 1:
                    # tiebreak: linear decode from the nearest preceding direct-call target
                    starts = [e for e in call_targets if e <= P]
                    if starts:
                        va = max(starts)
                        while va <= P:
                            i = img.decode(va)
                            if i is None: break
                            if va <= P < va + i.size:
                                hits = [h for h in hits if h[0] == va] or hits
                                break
                            va += i.size
                if len(hits) != 1:
                    problems.append(dict(writer=hex(wva), target=hex(P), size=sz,
                                         owners=[f'{t:x} {insns[t].mnemonic} {insns[t].op_str}' for t in owner.get(P, ())]))
                    continue
                t, kind, off, fsz = hits[0]
                if not any(x['off'] == off for x in sites[t]):
                    sites[t].append(dict(kind=kind, off=off, size=fsz))
                ti = insns[t]
                if kind == 'imm' and (ti.mnemonic.startswith('j') or ti.mnemonic in ('call', 'loop')):
                    # value written: immediate store, or the last `mov reg, imm` before the store
                    wi = insns[wva]; val = None
                    if wi.operands[1].type == X86_OP_IMM:
                        val = wi.operands[1].imm
                    else:
                        reg = wi.operands[1].reg
                        for k in range(pos[wva] - 1, max(-1, pos[wva] - 8), -1):
                            p = insns[order[k]]
                            if p.mnemonic == 'mov' and p.operands[0].type == X86_OP_REG and p.operands[0].reg == reg \
                                    and p.operands[1].type == X86_OP_IMM:
                                val = p.operands[1].imm; break
                    if val is None:
                        problems.append(dict(writer=hex(wva), target=hex(P), why='branch patch value not constant'))
                        continue
                    bits = 8 * fsz
                    v = val & ((1 << bits) - 1)
                    if v >> (bits - 1): v -= 1 << bits
                    branch[t].add((t + ti.size + v) & 0xFFFFFFFF)
                    branch[t].add(ti.operands[0].imm & 0xFFFFFFFF)   # original target
            changed = dict(branch) != self.smc_branch or dict(sites) != self.smc_sites
            self.smc_sites, self.smc_branch, self.smc_problems = dict(sites), {k: set(v) for k, v in branch.items()}, problems
            if not changed:
                break
        return writes

    def flags_analyze(self, max_iter=50):
        """interprocedural flag liveness: which flags cross call/return boundaries."""
        img = self.img
        fns = {}
        for e in self.entries:
            f = Fn(self, e); f.walk()
            info = {}
            for va, i in f.insns.items():
                if i is None: continue
                u, d = flag_usedef(i)
                kind, tgt = None, None
                m = i.mnemonic
                succ, ft = successors(img, i, self.entries, e)
                succ = list(succ) + [t for t in self.smc_branch.get(va, ()) if not (t in self.entries and t != e)]
                if ft: succ.append(va + i.size)
                if m == 'call':
                    op = i.operands[0]
                    if op.type == X86_OP_IMM: kind, tgt = 'call', op.imm & 0xFFFFFFFF
                    elif op.type == X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 and (op.mem.disp & 0xFFFFFFFF) in img.iat and not img.iat_indirect:
                        kind = 'import'
                    else: kind = 'icall'
                elif m in ('ret', 'retf'):
                    kind = 'ret'
                elif (m == 'jmp' or m.startswith('j') or m.startswith('loop')) and i.operands and i.operands[0].type == X86_OP_IMM:
                    t = i.operands[0].imm & 0xFFFFFFFF
                    if t in self.entries and t != e: kind, tgt = 'tail', t
                elif m == 'jmp' and not jump_table(img, i):
                    kind = 'itail'
                info[va] = (u, d, succ, kind, tgt)
            fns[e] = info
        self._fl_fns = fns
        fl_in, fl_out = collections.defaultdict(int), collections.defaultdict(int)
        ind_in = ind_out = 0
        for it in range(max_iter):
            changed = False
            for e, info in fns.items():
                live = {va: 0 for va in info}
                stable = False
                while not stable:
                    stable = True
                    for va in sorted(info, reverse=True):
                        u, d, succ, kind, tgt = info[va]
                        out = 0
                        for s in succ:
                            out |= live.get(s, 0)
                        if kind == 'call':
                            if out & ~fl_out[tgt]:
                                fl_out[tgt] |= out; changed = True
                            new = fl_in[tgt]
                        elif kind == 'icall':
                            new = 0   # indirect calls are flag boundaries (compiled code; asm uses direct calls)
                        elif kind == 'import':
                            new = 0
                        elif kind == 'ret':
                            new = fl_out[e]
                        elif kind == 'tail':
                            if fl_out[e] & ~fl_out[tgt]:
                                fl_out[tgt] |= fl_out[e]; changed = True
                            new = fl_in[tgt] | (u | (out & ~d))   # conditional tail: fallthrough too
                        elif kind == 'itail':
                            new = u | out
                        else:
                            new = u | (out & ~d)
                        if new != live[va]:
                            live[va] = new; stable = False
                ein = live.get(e, 0)
                if ein & ~fl_in[e]:
                    fl_in[e] |= ein; changed = True
            if not changed:
                break
        self.fl_in = {k: v for k, v in fl_in.items() if v}
        self.fl_out = {k: v for k, v in fl_out.items() if v}
        self.fl_ind_in, self.fl_ind_out = ind_in, ind_out
        return it

    def regs_analyze(self, max_iter=60):
        """--cache-regs: per function, which guest registers it (and everything it calls) may read and may write, taken
        from the C it is emitted as. Calls to the host, indirect calls and anything not understood count as all."""
        ALL = frozenset(_CR_NAMES)
        own, edges = {}, {}
        callre = re.compile(r'\b%s_([0-9a-f]{8})\(c\)' % self.prefix)
        for e in self.entries:
            f = Fn(self, e); f.walk()
            R, W, ed, unknown = set(), set(), set(), False
            for va, i in f.insns.items():
                if i is None: continue
                try: body = f.insn(i)
                except Unsupported: continue                   # becomes rt_fault (stores everything)
                for raw in body:
                    l = cache_regs(raw, self.prefix)
                    R |= set(_CR_TOK.findall(l)); W |= cache_regs_writes(l)
                    if any(k in l for k in ('rt_call_import', 'rt_cpuid', 'RT_SETJMP', 'rt_longjmp', f'{self.prefix}_call(c')):
                        unknown = True
                    ed |= {int(x, 16) for x in callre.findall(l)}
            R.add('esp'); W.add('esp')
            own[e] = ALL if unknown else frozenset(R), ALL if unknown else frozenset(W)
            edges[e] = ed
        sumR = {e: set(own[e][0]) for e in own}; sumW = {e: set(own[e][1]) for e in own}
        for it in range(max_iter):
            changed = False
            for e in own:
                for t in edges[e]:
                    if t not in own: sumR[e] |= ALL; sumW[e] |= ALL; continue
                    if not sumR[t] <= sumR[e]: sumR[e] |= sumR[t]; changed = True
                    if not sumW[t] <= sumW[e]: sumW[e] |= sumW[t]; changed = True
            if not changed: break
        self.reg_sum = {e: (frozenset(sumR[e]), frozenset(sumW[e])) for e in own}
        return it

    def fname(self, va):
        return f'{self.prefix}_{va:08x}'

    def impname(self, slot):
        d, n = self.img.imports[self.img.iat[slot]]
        return f'{d}!{n}'

    def run(self, outdir, per_file=150):
        os.makedirs(outdir, exist_ok=True)
        ents = sorted(self.entries)
        unimpl = collections.Counter()
        unimpl_funcs = collections.defaultdict(list)
        total_insns = 0
        hdr = [f'/* generated by darkrecomp */', '#include "rt_fast.h"' if self.cache_regs else '#include "rt.h"']
        decl = hdr + [f'void {self.prefix}_call(CPU *c, uint32_t t);'] + [f'void {self.fname(e)}(CPU *c);' for e in ents]
        open(os.path.join(outdir, f'{self.prefix}_decls.h'), 'w').write('\n'.join(decl) + '\n')
        files = []
        for fi in range(0, len(ents), per_file):
            chunk = ents[fi:fi + per_file]
            src = [f'#include "{self.prefix}_decls.h"', '']
            for e in chunk:
                f = Fn(self, e)
                f.walk()
                total_insns += len(f.insns)
                src.append(f.emit())
                src.append('')
                unimpl.update(f.unimpl)
                for k in f.unimpl:
                    unimpl_funcs[k].append(e)
            name = f'{self.prefix}_f{fi // per_file:03d}.c'
            open(os.path.join(outdir, name), 'w').write('\n'.join(src))
            files.append(name)
        # dispatch table
        t = [f'#include "{self.prefix}_decls.h"', '#include <stdlib.h>',
             f'typedef struct {{ uint32_t va; guest_fn fn; }} rc_ent;',
             f'const rc_ent {self.prefix}_table[] = {{']
        t += [f'    {{0x{e:08x}u, {self.fname(e)}}},' for e in ents]
        t += ['};', f'const unsigned {self.prefix}_table_n = {len(ents)};',
              f'guest_fn {self.prefix}_lookup(uint32_t va) {{',
              f'    unsigned lo = 0, hi = {len(ents)};',
              '    while (lo < hi) { unsigned mid = (lo + hi) / 2;',
              f'        if ({self.prefix}_table[mid].va < va) lo = mid + 1; else hi = mid; }}',
              f'    return (lo < {len(ents)} && {self.prefix}_table[lo].va == va) ? {self.prefix}_table[lo].fn : 0;',
              '}',
              f'void {self.prefix}_call(CPU *c, uint32_t t) {{',
              f'    guest_fn f = {self.prefix}_lookup(t);',
              '    if (f) f(c); else rt_call_external(c, t);',
              '}']
        open(os.path.join(outdir, f'{self.prefix}_table.c'), 'w').write('\n'.join(t) + '\n')
        files.append(f'{self.prefix}_table.c')
        meta = dict(prefix=self.prefix, base=self.img.base, entry=self.img.entry,
                    exports={hex(k): v for k, v in self.img.exports.items()},
                    imports=self.img.imports, iat={hex(k): v for k, v in self.img.iat.items()},
                    functions=len(ents), insns=total_insns, files=files,
                    unimpl=dict(unimpl.most_common()),
                    unimpl_funcs={k: [hex(x) for x in v[:20]] for k, v in unimpl_funcs.items()},
                    sections=[(n, a, s, c) for (n, a, s, c) in self.img.sections],
                    smc_sites={hex(k): v for k, v in self.smc_sites.items()},
                    smc_branch={hex(k): sorted(hex(x) for x in v) for k, v in self.smc_branch.items()},
                    smc_problems=self.smc_problems,
                    flags_in={hex(k): v for k, v in self.fl_in.items()}, flags_out={hex(k): v for k, v in self.fl_out.items()},
                    entries=ents)
        json.dump(meta, open(os.path.join(outdir, f'{self.prefix}_meta.json'), 'w'), indent=1)
        return meta


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('pe'); ap.add_argument('prefix'); ap.add_argument('outdir')
    ap.add_argument('--funcs-per-file', type=int, default=150)
    ap.add_argument('--smc', action='store_true', help='analyse and translate self-modifying code')
    ap.add_argument('--rebase', type=lambda v: int(v, 0), help='lift a DLL as if loaded at this base (applies its relocations first)')
    ap.add_argument('--cache-regs', action='store_true', help='keep guest registers in C locals inside each function (faster; needs runtime/rt_fast.h)')
    ap.add_argument('--iat-indirect', action='store_true', help='for DLLs loaded by Windows: call imports through their IAT slot as plain indirect calls (the slot holds the real address)')
    a = ap.parse_args()
    img = Image(a.pe, a.rebase)
    img.iat_indirect = a.iat_indirect   # imports stay known by name (setjmp/longjmp), but are called through the IAT
    L = Lifter(a.pe, a.prefix, img)
    L.cache_regs = a.cache_regs
    if a.smc:
        w = L.smc_analyze()
        print(f'SMC: {len(w)} code writes -> {sum(len(v) for v in L.smc_sites.values())} patched fields in '
              f'{len(L.smc_sites)} instructions, {len(L.smc_branch)} patched branches, {len(L.smc_problems)} unresolved')
        for p in L.smc_problems: print('   unresolved:', p)
    it = L.flags_analyze()
    print(f'flags: {len(L.fl_in)} functions read flags on entry, {len(L.fl_out)} export flags on return, '
          f'indirect in/out = {L.fl_ind_in:#x}/{L.fl_ind_out:#x} ({it + 1} iterations)')
    for e in sorted(set(L.fl_in) | set(L.fl_out))[:40]:
        print(f'   {e:08x} in={"".join(flagset(L.fl_in.get(e, 0)))} out={"".join(flagset(L.fl_out.get(e, 0)))}')
    if L.cache_regs:
        it = L.regs_analyze()
        full = sum(1 for r, w in L.reg_sum.values() if len(w) == 8)
        print(f'cache-regs: {len(L.reg_sum)} functions summarised, {full} may change every register ({it + 1} iterations)')
    meta = L.run(a.outdir, a.funcs_per_file)
    print(f"functions={meta['functions']} insns(with dup)={meta['insns']} files={len(meta['files'])}")
    print('unimpl:', meta['unimpl'])
