#!/usr/bin/env python3
"""apply_hooks.py [--hooks hooks.txt] <lifted dir>...   (e.g. out/nd out/ao out/sq)

Inserts calls to your own C functions at the entry of recompiled functions, in the generated C, in seconds (no
re-lift). hooks.txt has one hook per line:

    0x00601430  my_render_hook      # comment

which makes the recompiled function at 0x00601430 call `void my_render_hook(CPU *c)` first, every time it is entered
(from recompiled or native code). The hook sees the guest registers and stack in *c and may change them.

The original generated files are kept as *.c.orig; every run starts from them, so removing a line (or running without
a hooks file) removes the hook. Only files whose content changes are rewritten, so only those recompile."""
import sys, os, re, glob, argparse

ap = argparse.ArgumentParser(); ap.add_argument('--hooks')
ap.add_argument('--record', action='store_true', help='also call rec_enter(c, address) at the entry of every function of the main exe (prefix nd): the play-session recorder, host/record.inc')
ap.add_argument('dirs', nargs='+'); a = ap.parse_args()
hooks = {}
if a.hooks:
    for ln in open(a.hooks, encoding='utf-8'):
        p = ln.split('#')[0].split()
        if len(p) == 2: hooks[int(p[0], 16)] = p[1]
        elif p: sys.exit(f'bad hooks line: {ln.strip()}')

pat = re.compile(r'^void (?P<pfx>[a-z]+)_(?P<va>[0-9a-f]{8})\(CPU \*c\) \{$')
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
        if m and int(m.group('va'), 16) in hooks:
            va = int(m.group('va'), 16); found.add(va); touched = True
            out.append(f'    {{ extern void {hooks[va]}(CPU *); {hooks[va]}(c); }}   /* hook (tools/apply_hooks.py) */')
    new = '\n'.join(out)
    if touched and not os.path.exists(orig): os.replace(fn, orig)          # keep the pristine file once
    cur = open(fn, encoding='utf-8').read() if os.path.exists(fn) else None
    if cur != new:
        open(fn, 'w', encoding='utf-8').write(new); changed += 1
    if not touched and os.path.exists(orig): os.remove(orig)              # back to pristine: drop the copy
missing = set(hooks) - found
for va in sorted(missing): print(f'  hook target 0x{va:08x} is not the start of a recompiled function', file=sys.stderr)
print(f'hooks: {len(found)} applied, {changed} generated file(s) changed')
sys.exit(1 if missing else 0)
