"""lift_x87.py: keep the x87 register stack in C locals (lift.py --cache-regs).

The default translation models the x87 stack as c->st[8] indexed by the run-time top c->top: every ST(i) is a load of
c->top plus an indexed load or store in the CPU struct, every push and pop rewrites c->top, and since guest memory is
written through a byte pointer the C compiler must assume each guest store may have changed all of it. So x87-heavy
code (all of the Dark Engine's 3D math, physics and lighting) runs out of memory.

Here, inside one function, the stack depth at every instruction is known when lifting (compilers keep it static), so
each stack position becomes a named local: position p is the p-th value counted from the depth at function entry
(negative positions are values that were on the stack when the function was entered). ST(i) at depth d is position
d - 1 - i. The struct is the truth only outside the function: on entry the positions the function touches are read
from it (anchored on the run-time top, x87tb), and they are written back, with c->top, before every call, tail jump and
return, and before the few instructions that look at the raw state (fnstsw needs the top, fxam reads the struct).

Calls: by the MSVC calling convention the x87 stack is empty at a call and holds at most one value (a float result)
afterwards, so a call changes the depth by k = 0 or 1 (the _CI* math intrinsics take arguments on the stack: their k is
known by name). k comes from the callee's own analysis when it is a direct call to a function whose depth change is
known, otherwise from the code after the call (a float result is used or popped before anything is pushed). After the
call the positions are reloaded, and the new top is compared with what k predicts; a mismatch faults loudly at that call
site (RT_FAULT_X87) instead of computing wrong values. When no x87 instruction follows a call before the next call or
return, k doesn't matter: the struct stays the truth on that path (depth "unknown"), nothing is reloaded or written back.

A function keeps the old translation (and costs nothing extra) when the depth cannot be followed statically: a join of
paths at different depths, a conditional push or pop (fptan/fsincos on huge arguments), instructions that set the top
directly (fldenv, frstor, fnsave, fninit, fincstp, fdecstp), setjmp/longjmp, or more than eight positions in use.
lift.py --x87-exempt lists functions (or call sites) to leave alone if a fault ever names one.
"""
import re

ST_RE = re.compile(r'\bST\((\d+)\)')
PUSH_TOK = 'FPUSH(c, '
POP_TOK = 'FPOP(c);'
CI_K = {'_CIsqrt': 0, '_CIsin': 0, '_CIcos': 0, '_CItan': 0, '_CIasin': 0, '_CIacos': 0, '_CIatan': 0, '_CIexp': 0,
        '_CIlog': 0, '_CIlog10': 0, '_CIatan2': -1, '_CIpow': -1, '_CIfmod': -1}
BAD = ('c->top', 'ST(r_)', 'FPUSH(c, cos', 'rt_longjmp', 'RT_SETJMP')      # FPUSH(c, cos.. / tan: conditional (fsincos/fptan)
FAULT_X87 = 6


def _ineligible_text(t):
    return any(b in t for b in BAD) or ('9223372036854775808' in t and (PUSH_TOK in t or POP_TOK in t))


def _close(s, k):
    """index just past the parenthesis that closes the one opened right before s[k]"""
    depth = 1
    while k < len(s):
        if s[k] == '(': depth += 1
        elif s[k] == ')':
            depth -= 1
            if depth == 0: return k + 1
        k += 1
    return -1


def _effects(text):
    """an instruction's x87 effect in order: list of ('r', i) reads of ST(i), ('push',) and ('pop',), as written"""
    ev = []
    k = 0
    while k < len(text):
        m = ST_RE.match(text, k)
        if m: ev.append(('r', int(m.group(1)))); k = m.end(); continue
        if text.startswith(PUSH_TOK, k):
            e = _close(text, k + len(PUSH_TOK))
            ev.extend(_effects(text[k + len(PUSH_TOK):e - 1])); ev.append(('push',)); k = e; continue
        if text.startswith(POP_TOK, k):
            ev.append(('pop',)); k += len(POP_TOK); continue
        k += 1
    return ev


def _call_kind(L, i, text):
    """('call', target or None, import name or None) for a call; ('tail', ...) for a jump into another function"""
    m = i.mnemonic
    if m not in ('call', 'jmp') and not m.startswith('j'):
        return None
    pfx = L.prefix
    calls = re.search(r'\b%s_[0-9a-f]{8}\(c\);|\b%s_call\(c, t\);|\brt_call_import\(c, (\d+)\);' % (pfx, pfx), text)
    if not calls:
        return None
    imp = L.img.imports[int(calls.group(1))][1] if calls.group(1) else None
    tgt = int(re.search(r'%s_([0-9a-f]{8})' % pfx, calls.group(0)).group(1), 16) if calls.group(0).startswith(pfx + '_') \
        and not calls.group(0).startswith(pfx + '_call') else None
    return ('tail' if 'return;' in text[calls.end():] else 'call', tgt, imp)


def _no(L, e, why):
    L.x87_declined[e] = why
    return None


def analyze(L, e, insns_text, succ, nets):
    """analyse_once(), and when a hands-off call ('free': nothing x87 between it and a return) makes paths meet at an
    unknown and a known depth, again with those calls assumed to change the depth by 0, then by 1: the other path at the
    join says which (and the run-time check after the call verifies it)"""
    r = analyse_once(L, e, insns_text, succ, nets, None)
    if r is None and 'and None' in L.x87_declined.get(e, '') or r is None and 'depths None' in L.x87_declined.get(e, ''):
        for forced in (0, 1):
            r = analyse_once(L, e, insns_text, succ, nets, forced)
            if r is not None:
                L.x87_declined.pop(e, None); break
    return r


def analyse_once(L, e, insns_text, succ, nets, force_free):
    """one function -> dict(eligible, depth{va}, k{va}, lo, hi, net) or None. insns_text: va -> emitted C of that
    instruction (before --cache-regs rewriting), succ: va -> successors inside the function."""
    if e in L.x87_exempt or e in L.setjmp_fns or e in L.longjmp_fns:
        return _no(L, e, 'exempt or setjmp/longjmp')
    if not any(ST_RE.search(t) or PUSH_TOK in t or POP_TOK in t for t in insns_text.values()):
        return None                                                  # no x87 at all: nothing to do
    eff, kind = {}, {}
    for va, t in insns_text.items():
        if _ineligible_text(t): return _no(L, e, 'sets the top directly / conditional push / setjmp')
        if va in L.x87_exempt: return _no(L, e, 'exempt call site')                          # a call site named in --x87-exempt
        eff[va] = _effects(t)
        kind[va] = _call_kind(L, L.img.decode(va), t)
    def evidence(starts):
        """the call's depth change k from the code after it: the smallest k in (0, 1) for which nothing below the depth at
        the call is read or popped before the next call or return. With no x87 instruction before the next call or
        return: 'free' when every path reaches a return with no call in between (a wrapper handing on a float result:
        the struct stays the truth), else 0 (MSVC consumes a float result before the next call; checked at run time)"""
        any_x87, hits_call = False, False
        for k in (0, 1):
            ok, seen, work = True, set(), [(s, k) for s in starts]
            while work and ok:
                va, d = work.pop()
                if (va, d) in seen or abs(d) > 16: continue
                seen.add((va, d))
                for x in eff.get(va, ()):
                    any_x87 = True
                    if x[0] == 'r':
                        if d - 1 - x[1] < 0: ok = False; break
                    elif x[0] == 'push': d += 1
                    else:
                        if d - 1 < 0: ok = False; break
                        d -= 1
                if not ok: continue
                if kind.get(va):
                    hits_call = True; continue
                if L.img.decode(va).mnemonic in ('ret', 'retf'): continue
                for s in succ.get(va, ()): work.append((s, d))
            if not any_x87: return 0 if hits_call else 'free'
            if ok: return k
        return 'bad'

    depth = {e: 0}
    work = [e]
    kk = {}
    lo, hi = 0, 0
    net = set()
    while work:
        va = work.pop()
        d = depth[va]
        for x in eff.get(va, ()):
            if d is None: return _no(L, e, f'x87 use at {va:x} after a call whose depth change is unknown')
            if x[0] == 'r': p = d - 1 - x[1]; lo, hi = min(lo, p), max(hi, p)
            elif x[0] == 'push': lo, hi = min(lo, d), max(hi, d); d += 1
            else: d -= 1
        ck = kind.get(va)
        if ck and ck[0] == 'call':                                   # a tail jump leaves (written back before it); a
            tgt, imp = ck[1], ck[2]                                  # conditional one falls through at the same depth
            if d is None: k = 'free'
            elif imp in CI_K: k = CI_K[imp]
            elif tgt is not None and nets.get(tgt) is not None: k = nets[tgt]
            else:
                k = evidence(succ.get(va, []))
                if k == 'bad': return _no(L, e, f'call at {va:x}: depth change not inferable')
                if k == 'free' and force_free is not None: k = force_free
            kk[va] = k
            if k == 'free': d = None
            else:
                if k > 0: lo, hi = min(lo, d), max(hi, d + k - 1)
                if k < 0 and d + k < 0: lo = min(lo, d + k)
                d = d + k
        m = L.img.decode(va).mnemonic
        if m in ('ret', 'retf'):
            net.add(d); continue
        for s in succ.get(va, []):
            if s not in depth:
                depth[s] = d; work.append(s)
            elif depth[s] != d:
                return _no(L, e, f'paths meet at {s:x} with depths {depth[s]} and {d}')
    if hi - lo >= 8:
        return _no(L, e, 'more than 8 stack positions')
    nets_out = {n for n in net if n is not None}
    return dict(depth=depth, k=kk, lo=lo, hi=hi, net=nets_out.pop() if len(nets_out) == 1 and None not in net else None)


def _pname(p):
    return f'x87_{p}' if p >= 0 else f'x87_m{-p}'


def _phys(p):
    return f'((x87tb - ({p}) - 1) & 7u)'


def flush(info, d):
    """write the positions and the top back to the CPU struct, as one expression (so it fits in front of a fault in an if)"""
    if d is None: return '((void)0)'
    st = ', '.join(f'c->st[{_phys(p)}] = {_pname(p)}' for p in range(info['lo'], info['hi'] + 1))
    return f'((void)({st}, c->top = (x87tb - ({d})) & 7u))'


def reload(info, va, d_after):
    chk = f'if (c->top != ((x87tb - ({d_after})) & 7u)) rt_fault(c, 0x{va:x}u, {FAULT_X87}); '
    ld = ' '.join(f'{_pname(p)} = c->st[{_phys(p)}];' for p in range(info['lo'], info['hi'] + 1))
    return f' {{ {chk}{ld} }}'


def prologue(info):
    decl = ', '.join(f'{_pname(p)} = c->st[{_phys(p)}]' for p in range(info['lo'], info['hi'] + 1))
    return f'    uint32_t x87tb = c->top; double {decl}; (void)x87tb;'


def rewrite_insn(L, info, va, text):
    """one instruction's emitted C, with ST()/FPUSH/FPOP turned into the position locals and the struct written back /
    read back around calls, returns and raw-state instructions"""
    d = info['depth'].get(va)
    if d is None:
        return text                                                  # only on paths where no x87 code runs
    out, k = [], 0
    pfx = L.prefix
    callre = re.compile(r'\b%s_[0-9a-f]{8}\(c\);|\b%s_call\(c, t\);|\brt_call_import\(c, \d+\);' % (pfx, pfx))
    done_sync = False
    while k < len(text):
        m = ST_RE.match(text, k)
        if m:
            out.append(_pname(d - 1 - int(m.group(1)))); k = m.end(); continue
        if text.startswith(PUSH_TOK, k):
            e = _close(text, k + len(PUSH_TOK))
            inner = rewrite_expr(text[k + len(PUSH_TOK):e - 1], d)
            out.append(f'({_pname(d)} = ({inner}))'); d += 1; k = e
            if text.startswith(';', k): out.append(';'); k += 1
            continue
        if text.startswith(POP_TOK, k):
            d -= 1; k += len(POP_TOK); continue
        if text.startswith('FXAM(c);', k):
            out.append(flush(info, d) + '; FXAM(c);'); k += len('FXAM(c);'); continue
        if text.startswith('rt_fault(c, ', k):
            # write back inside the address argument: rt_fault(c, (FLUSH, addr), kind), so it stays one call (the
            # --cache-regs pass wraps rt_fault(c, ...); in a block, and it may sit in an if without braces)
            a0 = k + len('rt_fault(c, '); j, depth_ = a0, 0
            while j < len(text) and not (text[j] == ',' and depth_ == 0):
                depth_ += {'(': 1, ')': -1}.get(text[j], 0); j += 1
            out.append(f'rt_fault(c, ({flush(info, d)}, {text[a0:j]})'); k = j; continue
        cm = callre.match(text, k)
        if cm:
            tail = 'return;' in text[cm.end():]
            out.append(flush(info, d) + '; ' + cm.group(0))
            k = cm.end()
            if not tail:
                kc = info['k'].get(va)
                if kc not in (None, 'free'):
                    d = d + kc
                    out.append(reload(info, va, d))
            done_sync = True
            continue
        if text.startswith('return;', k) and not done_sync:
            out.append(flush(info, d) + '; return;'); k += len('return;'); continue
        out.append(text[k]); k += 1
    res = ''.join(out)
    if 'FSW(c)' in text:                                             # fnstsw: the status word carries the top
        res = f'c->top = (x87tb - ({info["depth"][va]})) & 7u; ' + res
    return res


def rewrite_expr(s, d):
    return ST_RE.sub(lambda m: _pname(d - 1 - int(m.group(1))), s)
