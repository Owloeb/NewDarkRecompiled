#!/usr/bin/env python3
"""apply_hooks.py [--hooks hooks.txt] <lifted dir>...   (e.g. out/nd out/ao out/sq)

Inserts calls to your own C functions at the entry of recompiled functions, in the generated C, in seconds (no
re-lift). hooks.txt has one hook per line:

    0x00601430  my_render_hook      # comment

which makes the recompiled function at 0x00601430 call `void my_render_hook(CPU *c)` first, every time it is entered
(from recompiled or native code). The hook sees the guest registers and stack in *c and may change them.

The original generated files are kept as *.c.orig; every run starts from them, so removing a line (or running without
a hooks file) removes the hook. Only files whose content changes are rewritten, so only those recompile."""
import sys, os, re, glob, argparse, json

ap = argparse.ArgumentParser(); ap.add_argument('--hooks')
ap.add_argument('--record', action='store_true', help='also call rec_enter(c, address) at the entry of every function of the main exe (prefix nd): the play-session recorder, host/record.inc')
ap.add_argument('--fastpaths', metavar='FILE', help='port/core/fastpaths.c: every `int fp_XXXXXXXX(CPU *c)` in it is tried first on entry to recompiled function XXXXXXXX (a nonzero return means it did the whole call); see that file')
ap.add_argument('dirs', nargs='+'); a = ap.parse_args()
hooks = {}
if a.hooks:
    for ln in open(a.hooks, encoding='utf-8'):
        p = ln.split('#')[0].split()
        if len(p) == 2: hooks[int(p[0], 16)] = p[1]
        elif p: sys.exit(f'bad hooks line: {ln.strip()}')

fast = set(int(m, 16) for m in re.findall(r'^int fp_([0-9a-f]{8})\(CPU \*c\)', open(a.fastpaths).read(), re.M)) if a.fastpaths else set()


def check_fastpaths_vs_cache_regs(path, dirs):
    """Code lifted with --cache-regs keeps registers in C locals, and a caller stores and reloads only the registers its
    callee may read or write (the lifter's per-function summary, reg_sum in <prefix>_meta.json). A fast path runs at the
    callee's entry, where the CPU struct is in sync, so it is safe exactly when it reads only registers in the summary's read
    set and writes only registers in its write set. Checked here from the fast path's source; returns a list of problems."""
    sums = {}
    for d in dirs:
        for mf in glob.glob(os.path.join(d, '*_meta.json')):
            sums.update({int(k, 16): (set(r.split()), set(w.split())) for k, (r, w) in json.load(open(mf)).get('reg_sum', {}).items()})
    if not sums: return []                                                       # not lifted with --cache-regs: nothing to check
    src = re.sub(r'/\*.*?\*/|//[^\n]*', '', open(path, encoding='utf-8').read(), flags=re.S)
    regs = r'(eax|ecx|edx|ebx|esp|ebp|esi|edi)'
    wr = re.compile(r'\bc->' + regs + r'\s*=(?!=)')                               # plain store: a write, not a read
    rmw = re.compile(r'\bc->' + regs + r'\s*(?:[-+*/%&|^]|<<|>>)=|(?:\+\+|--)\s*c->' + regs + r'\b|\bc->' + regs + r'\s*(?:\+\+|--)')
    known = {'RETCHK', 'FPOP', 'FPUSH', 'FCOM', 'FXAM', 'FSW', 'F2I16', 'F2I32', 'F2I64', 'FROUND'}   # rt.h: esp / x87 only
    problems = []
    for m in re.finditer(r'^int fp_([0-9a-f]{8})\(CPU \*c\) \{\n(.*?)^\}', src, re.S | re.M):
        va, body = int(m.group(1), 16), m.group(2)
        if va not in sums: continue                                              # reported as a missing target below
        R, W = sums[va]
        writes = set(wr.findall(body)) | {x for t in rmw.findall(body) for x in t if x}
        reads = set(re.findall(r'\bc->' + regs, wr.sub('', body)))
        for f in set(re.findall(r'\b(\w+)\(\s*c\s*[,)]', body)) - known:
            problems.append(f'fp_{va:08x} passes the CPU to {f}(), whose register use this check cannot see')
        for r in sorted(reads - R): problems.append(f'fp_{va:08x} reads {r}, which the lifted function never reads (callers may not have stored it)')
        for r in sorted(writes - W): problems.append(f'fp_{va:08x} writes {r}, which the lifted function never writes (callers will not reload it)')
    return problems


if a.fastpaths:
    bad = check_fastpaths_vs_cache_regs(a.fastpaths, a.dirs)
    if bad:
        sys.exit('fast paths do not fit the --cache-regs register summaries:\n  ' + '\n  '.join(bad))
pat =re.compile(r'^void (?P<pfx>[a-z]+)_(?P<va>[0-9a-f]{8})\(CPU \*c\) \{$')
found, changed = set(), 0
for fn in sorted(f for d in a.dirs for f in glob.glob(os.path.join(d, '*.c'))):
    orig = fn + '.orig'
    base = open(orig if os.path.exists(orig) else fn, encoding='utf-8').read()
    out, touched = [], False
    for ln in base.split('\n'):
        out.append(ln)
        m = pat.match(ln)
        if a.record and m and m.group('pfx') == 'nd':
            touched = True
            out.append(f'    {{ extern void rec_enter(CPU *, uint32_t); rec_enter(c, 0x{m.group("va")}u); }}   /* recorder (--record) */')
        if m and m.group('pfx') == 'nd' and int(m.group('va'), 16) in fast:
            touched = True; found.add(int(m.group('va'), 16))
            out.append(f'    {{ extern int fp_{m.group("va")}(CPU *); if (fp_{m.group("va")}(c)) return; }}   /* fast path (port/core/fastpaths.c) */')
        if m and int(m.group('va'), 16) in hooks:
            va = int(m.group('va'), 16); found.add(va); touched = True
            out.append(f'    {{ extern void {hooks[va]}(CPU *); {hooks[va]}(c); }}   /* hook (tools/apply_hooks.py) */')
    new = '\n'.join(out)
    if touched and not os.path.exists(orig): os.replace(fn, orig)          # keep the pristine file once
    cur = open(fn, encoding='utf-8').read() if os.path.exists(fn) else None
    if cur != new:
        open(fn, 'w', encoding='utf-8').write(new); changed += 1
    if not touched and os.path.exists(orig): os.remove(orig)              # back to pristine: drop the copy
missing = (set(hooks) | fast) - found
for va in sorted(missing): print(f'  hook target 0x{va:08x} is not the start of a recompiled function', file=sys.stderr)
print(f'hooks/fast paths: {len(found)} applied, {changed} generated file(s) changed')
sys.exit(1 if missing else 0)
