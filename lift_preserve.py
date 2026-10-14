"""lift_preserve.py: callee-saved register preservation for lift.py --cache-regs.

A function that saves EBX/EBP/ESI/EDI and restores them before returning (push ebx ... pop ebx; ret) still *writes* them,
so its register summary said "may change ebx" and every caller reloaded EBX after calling it. analyze() proves, per
function, which of the four it hands back unchanged on every exit. lift.py leaves those out of the function's write
summary, so callers keep them in C locals across the call. (MH3's PowerPC equivalent is its non-volatile residency.)

The proof follows every path through the function, tracking each register symbolically (its entry value, or the entry
stack pointer plus a constant) and the 4-byte stack slots written at known offsets. At every exit (ret, or a tail jump
into another function) the stack pointer must be back at its entry value and the register must hold its entry value.

- A direct call keeps a register if the callee is proven to preserve it, or never changes it at all.
- An indirect call or an import keeps all four (the calling-convention contract, lift.py _ABI_W). How many argument bytes
  it pops is read from the code around it: a following `add esp, k` means cdecl (pops none), otherwise the callee pops
  what was pushed for it (stdcall/thiscall). A wrong guess leaves the stack pointer off at the exit, so the proof fails.
- Every function starts out assumed to preserve all four and is disproved until nothing changes; that is sound by
  induction on call depth, so recursion is fine.
- setjmp/longjmp, cpuid, calls the lifter can't place, and instructions it can't translate prove nothing.

Assumed, not proven: a stack slot holding a saved register is not overwritten through a pointer the analysis cannot
follow (that takes a stack buffer overflow). An RT_ABI_CHECK build verifies every claim at run time at each call site
that relies on one (RT_ABICHKM in runtime/rt_fast.h).
"""
import collections
from capstone.x86 import X86_OP_REG, X86_OP_IMM, X86_OP_MEM, X86_REG_FS

PRES = ('ebx', 'ebp', 'esi', 'edi')
MASK = {'ebx': 1, 'ebp': 2, 'esi': 4, 'edi': 8}          # bit layout of RT_ABICHKM's mask
R32 = ('eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi')
_R16 = {'ax': 'eax', 'cx': 'ecx', 'dx': 'edx', 'bx': 'ebx', 'sp': 'esp', 'bp': 'ebp', 'si': 'esi', 'di': 'edi'}
_R8 = {'al': 'eax', 'cl': 'ecx', 'dl': 'edx', 'bl': 'ebx', 'ah': 'eax', 'ch': 'ecx', 'dh': 'edx', 'bh': 'ebx'}
_STRW = ('movsb', 'movsw', 'movsd', 'stosb', 'stosw', 'stosd')


def _full(i, rid):
    n = i.reg_name(rid)
    return n if n in R32 else _R16.get(n) or _R8.get(n)


def _r32(i, o):
    return o.type == X86_OP_REG and i.reg_name(o.reg) in R32


def _addr(L, i, op):
    """memory operand -> (base register or None, displacement); None when it isn't plain [reg+disp] / [disp]"""
    m = op.mem
    if m.segment == X86_REG_FS or m.index or i.address in L.smc_sites:
        return None
    return (i.reg_name(m.base) if m.base else None, m.disp)


def compact(L, e, H):
    """one function -> [(va, op, arg, successors)], decoded once so the fixpoint doesn't decode again.
    H: lift.py's Fn, Unsupported, successors and jump_table (passed in, so this works however lift.py was started)"""
    Fn, Unsupported, successors, jump_table = H['Fn'], H['Unsupported'], H['successors'], H['jump_table']
    f = Fn(L, e); f.walk()
    out = []
    for va in sorted(f.insns):
        i = f.insns[va]
        if i is None:
            out.append((va, 'stop', None, ())); continue
        try:
            f.insn(i)                                        # untranslatable: becomes a fault, the path ends there
        except Unsupported:
            out.append((va, 'stop', None, ())); continue
        tg, ft = successors(L.img, i, L.entries, e)
        succ = [t for t in list(tg) + list(L.smc_branch.get(va, ())) if t in f.insns]
        if ft and va + i.size in f.insns: succ.append(va + i.size)
        m = i.mnemonic.replace('lock ', '')
        ops = i.operands
        rec = None
        if m in ('ret', 'retf'):
            rec = ('ret', ops[0].imm if ops else 0)
        elif m in ('hlt', 'int3', 'ud2', 'iretd'):
            rec = ('stop', None)
        elif m == 'call':
            op = ops[0]
            n = L.img.decode(va + i.size)
            cdecl = n is not None and n.mnemonic == 'add' and _r32(n, n.operands[0]) and \
                n.reg_name(n.operands[0].reg) == 'esp' and n.operands[1].type == X86_OP_IMM
            if op.type == X86_OP_IMM:
                t = op.imm & 0xFFFFFFFF
                rec = ('call_d', (t, cdecl)) if (t in L.entries and t not in L.setjmp_fns and t not in L.longjmp_fns) \
                    else ('call_x', None)
            elif L.abi_call_target(i) is not None:
                rec = ('call_i', cdecl)
            else:
                rec = ('call_x', None)
        elif m == 'jmp':
            op = ops[0]
            if op.type == X86_OP_IMM:
                t = op.imm & 0xFFFFFFFF
                if any(fd['kind'] == 'imm' for fd in L.smc_sites.get(va, ())):
                    # a branch the game patches at run time: follow every target it can be patched to (they are in
                    # succ); if one of them is another function, it is a tail jump this proof doesn't follow
                    rec = ('smc_out', None) if any(x in L.entries and x != e for x in L.smc_branch.get(va, set()) | {t}) else ('nop', None)
                elif t in L.entries and t != e: rec = ('tail_d', t)
                else: rec = ('nop', None)
            elif jump_table(L.img, i):
                # a switch: its targets are in succ. The generated default case only runs if the table holds an address
                # the lifter never found, which faults loudly elsewhere, so it is not an exit of the function
                rec = ('nop', None)
            elif L.abi_call_target(i) is not None:
                rec = ('tail_i', None)
            else:
                rec = ('stop', None)
        elif m.startswith('j') and ops and ops[0].type == X86_OP_IMM and m != 'jecxz':
            t = ops[0].imm & 0xFFFFFFFF
            if any(x in L.entries and x != e for x in L.smc_branch.get(va, ())): rec = ('smc_out', None)
            else: rec = ('jcc_tail', t) if (t in L.entries and t != e) else ('nop', None)
        elif m == 'push' and ops[0].size == 4:
            o = ops[0]
            rec = ('push', ('r', i.reg_name(o.reg)) if _r32(i, o) else ('m', _addr(L, i, o)) if o.type == X86_OP_MEM else None)
        elif m == 'pop' and ops[0].size == 4:
            o = ops[0]
            rec = ('pop', ('r', i.reg_name(o.reg)) if _r32(i, o) else ('m', _addr(L, i, o)))
        elif m in ('pushfd', 'pushf'):
            rec = ('push', None)
        elif m in ('popfd', 'popf'):
            rec = ('pop', None)
        elif m == 'mov' and ops[0].type == X86_OP_MEM:
            s = ops[1]
            rec = ('store', (_addr(L, i, ops[0]), ops[0].size, i.reg_name(s.reg) if _r32(i, s) else None))
        elif m == 'mov' and _r32(i, ops[0]):
            d, s = i.reg_name(ops[0].reg), ops[1]
            if _r32(i, s): rec = ('copy', (d, i.reg_name(s.reg)))
            elif s.type == X86_OP_MEM: rec = ('load', (d, _addr(L, i, s)))
            else: rec = ('set', d)
        elif m == 'lea' and _r32(i, ops[0]):
            rec = ('lea', (i.reg_name(ops[0].reg), _addr(L, i, ops[1])))
        elif m in ('add', 'sub') and _r32(i, ops[0]) and ops[1].type == X86_OP_IMM:
            k = ops[1].imm if m == 'add' else -ops[1].imm
            rec = ('addk', (i.reg_name(ops[0].reg), ((k + 0x80000000) & 0xFFFFFFFF) - 0x80000000))
        elif m == 'leave':
            rec = ('leave', None)
        elif m == 'xchg' and all(_r32(i, o) for o in ops):
            rec = ('xchg', (i.reg_name(ops[0].reg), i.reg_name(ops[1].reg)))
        if rec is None:                                    # anything else: what it writes becomes unknown
            try:
                wr = {_full(i, r) for r in i.regs_access()[1]} - {None}
            except Exception:
                wr = set(R32)
            memw = tuple((_addr(L, i, o), o.size) for o in ops if o.type == X86_OP_MEM and (o.access & 2))
            strw = m.split()[-1] in _STRW
            rec = ('clob', (frozenset(wr), memw, strw))
        out.append((va, rec[0], rec[1], tuple(succ)))
    return out


def _pop_counts(bodies):
    """argument bytes each function pops on return (ret n), when the same on every exit, tail jumps included"""
    popn = {}
    for e, body in bodies.items():
        ns = {a for (_, op, a, _) in body if op == 'ret'}
        popn[e] = ns.pop() if len(ns) == 1 else None
    for _ in range(30):
        changed = False
        for e, body in bodies.items():
            tails = {a for (_, op, a, _) in body if op in ('tail_d', 'jcc_tail')}
            if not tails: continue
            vals = ({popn[e]} if any(op == 'ret' for (_, op, _, _) in body) else set()) | {popn.get(t) for t in tails}
            v = vals.pop() if len(vals) == 1 else None
            if v != popn[e]: popn[e] = v; changed = True
        if not changed: break
    return popn


def analyze(L, may_write, H):
    """function -> frozenset of the callee-saved registers it provably hands back unchanged. may_write(t, r): can function
    t (its own code or anything it calls) change register r at all. H: see compact()."""
    bodies = {e: compact(L, e, H) for e in L.entries}
    popn = _pop_counts(bodies)
    # setjmp/longjmp, and functions listed in --abi-exempt (a checked build caught them breaking a claim), claim nothing
    none = {e for e in L.entries if e in L.setjmp_fns or e in L.longjmp_fns or e in L.abi_exempt}
    P = {e: (frozenset() if e in none else frozenset(PRES)) for e in L.entries}
    callers = collections.defaultdict(set)
    for e, body in bodies.items():
        for (_, op, a, _) in body:
            if op == 'call_d': callers[a[0]].add(e)
            elif op in ('tail_d', 'jcc_tail'): callers[a].add(e)
    work, queued, steps = sorted(L.entries), set(L.entries), 0
    while work:
        e = work.pop(); queued.discard(e); steps += 1
        if e in none: continue
        new = prove(e, bodies[e], P, popn, may_write) & P[e]
        if new != P[e]:
            P[e] = new
            for c in callers[e]:
                if c not in queued: work.append(c); queued.add(c)
    return P, steps


def prove(e, body, P, popn, may_write, why=None):
    """the callee-saved registers function e restores on every exit, given the current claims for its callees"""
    ok = set(PRES)
    at = {va: k for k, (va, *_r) in enumerate(body)}
    if e not in at:
        return frozenset()
    regs0 = {r: ('E', r) for r in R32}; regs0['esp'] = ('S', 0)
    states = {e: (regs0, {}, 0)}               # registers, stack slots (offset -> value), bytes pushed for the next call
    work = [e]

    def kept_by(t, r):
        return r in P.get(t, ()) or not may_write(t, r)

    def check_exit(regs, t=None):
        if why is not None and not all(regs.get('esp') == ('S', 0) and regs.get(r) == ('E', r) for r in PRES):
            why.append((va, regs.get('esp'), {r: regs.get(r) for r in PRES}))
        for r in PRES:
            if not (regs.get('esp') == ('S', 0) and regs.get(r) == ('E', r) and (t is None or kept_by(t, r))):
                ok.discard(r)

    visits = 0
    while work and ok:
        va = work.pop()
        visits += 1
        if visits > 100000: return frozenset()                     # pathological control flow: prove nothing
        regs, slots, pend = states[va]
        regs, slots = dict(regs), dict(slots)
        _, op, a, succ = body[at[va]]
        sp = regs.get('esp')
        spo = sp[1] if sp and sp[0] == 'S' else None

        def addr(ad):
            if ad is None or ad[0] is None: return None
            v = regs.get(ad[0])
            return v[1] + ad[1] if v and v[0] == 'S' else None

        def store(ad, size, val):
            # a store to an address the analysis cannot resolve is assumed not to hit a saved-register slot (see the
            # module comment; RT_ABI_CHECK verifies the claims that rest on it)
            o = addr(ad)
            if o is None:
                return
            for k in [k for k in slots if k < o + size and o < k + 4]: del slots[k]
            if size == 4 and val is not None: slots[o] = val

        def drop_below(o):
            for k in [k for k in slots if o is None or k < o]: del slots[k]

        def set_esp(v):
            # slots below a known stack pointer are dead; with the stack pointer unknown the slots stay: compiled code
            # never writes into its own saved-register area, and a frame-pointer epilogue finds them again through EBP
            regs['esp'] = v
            if v and v[0] == 'S': drop_below(v[1])

        cont = True
        if op == 'stop':
            cont = False
        elif op == 'ret':
            check_exit(regs); cont = False
        elif op == 'tail_d':
            check_exit(regs, a); cont = False
        elif op == 'tail_i':
            check_exit(regs); cont = False
        elif op == 'smc_out':
            ok.clear()
        elif op == 'jcc_tail':
            check_exit(regs, a)
        elif op == 'push':
            val = None
            if a and a[0] == 'r': val = regs.get(a[1])
            elif a and a[0] == 'm':
                o = addr(a[1]); val = slots.get(o) if o is not None else None
            if spo is None: regs['esp'] = None
            else: regs['esp'] = ('S', spo - 4); store(('esp', 0), 4, val)
            # saving a callee-saved register (its entry value) is the prologue, not an argument for the next call
            if not (val and val[0] == 'E' and val[1] in ('ebx', 'ebp', 'esi', 'edi')):
                pend = None if pend is None else pend + 4
        elif op == 'pop':
            val = slots.get(spo) if spo is not None else None
            set_esp(('S', spo + 4) if spo is not None else None)
            if a and a[0] == 'r': regs[a[1]] = val
            elif a and a[0] == 'm': store(a[1], 4, val)
            pend = None if pend is None else max(0, pend - 4)
        elif op == 'store':
            ad, size, src = a
            store(ad, size, regs.get(src) if src else None)
        elif op == 'copy':
            d, s = a
            if d == 'esp': set_esp(regs.get(s)); pend = 0
            else: regs[d] = regs.get(s)
        elif op == 'load':
            d, ad = a
            o = addr(ad); v = slots.get(o) if o is not None else None
            if d == 'esp': set_esp(v); pend = 0
            else: regs[d] = v
        elif op == 'set':
            if a == 'esp': set_esp(None); pend = None
            else: regs[a] = None
        elif op == 'lea':
            d, ad = a
            o = addr(ad); v = ('S', o) if o is not None else None
            if d == 'esp': set_esp(v); pend = 0
            else: regs[d] = v
        elif op == 'addk':
            r, k = a
            v = regs.get(r)
            nv = ('S', v[1] + k) if v and v[0] == 'S' else None
            if r == 'esp':
                set_esp(nv)
                pend = 0 if k < 0 else (None if pend is None else max(0, pend - k))   # sub esp: locals, not arguments
            else:
                regs[r] = nv
        elif op == 'leave':
            v = regs.get('ebp')
            if v and v[0] == 'S':
                regs['ebp'] = slots.get(v[1]); set_esp(('S', v[1] + 4))
            else:
                regs['ebp'] = None; set_esp(None)
            pend = 0
        elif op == 'xchg':
            x, y = a
            regs[x], regs[y] = regs.get(y), regs.get(x)
            if 'esp' in a: pend = None; drop_below(None)
        elif op in ('call_d', 'call_i', 'call_x'):
            if op == 'call_d':
                t, cdecl = a
                keep = {r for r in PRES if kept_by(t, r)}; n = popn.get(t)
                if n is None: n = 0 if cdecl else pend          # e.g. an import thunk (jmp [IAT]): no ret of its own
            elif op == 'call_i':
                keep = set(PRES); n = 0 if a else pend
            else:
                keep = set(); n = None
            for r in R32:
                if r != 'esp' and r not in keep: regs[r] = None
            set_esp(('S', spo + n) if spo is not None and n is not None else None)
            pend = 0
        elif op == 'clob':
            rs, memw, strw = a
            for ad, size in memw: store(ad, size, None)
            if strw:                          # rep stos/movs: a buffer, never the saved-register area (same assumption)
                pass
            for r in rs:
                if r == 'esp': set_esp(None); pend = None
                else: regs[r] = None
        if not cont: continue
        st = (regs, slots, pend)
        for s in succ:
            old = states.get(s)
            if old is None:
                states[s] = st; work.append(s); continue
            jr = {k: v for k, v in old[0].items() if regs.get(k) == v}
            js = {k: v for k, v in old[1].items() if slots.get(k) == v}
            jp = old[2] if old[2] == pend else None
            if len(jr) != len(old[0]) or len(js) != len(old[1]) or jp != old[2]:
                states[s] = (jr, js, jp); work.append(s)
    return frozenset(ok)
