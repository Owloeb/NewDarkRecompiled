#!/usr/bin/env python3
"""suggest_names.py <darkrecomp_record.txt> <out/nd/nd_meta.json> [--sym file.sym ...] [--tags tags.txt]
                   [--report report.md] [--emit recorded.sym]

Reads the file the play-session recorder wrote (build_win.py --record, host/record.inc) and turns it into hints for naming the
engine's functions:

  * report.md: what ran in each activity tag only (the functions that belong to "shooting", "inventory", ...), the hottest functions,
    unnamed functions that call into Windows/Direct3D/DirectSound/DirectInput (with the file names and strings they pass), functions
    that were handed recognisable strings, and the call graph around them. Names already known (--sym files) are shown next to the
    addresses, so the report is a work list: what to name next and the evidence.
  * recorded.sym (--emit): the same evidence as notes, one line per executed function, in the symbol-file format
    (`0x<address> func - # [rec] ...`, name "-" = notes only). tools/name_sources.py puts them above each function in out/nd_named.

tags.txt (optional): one line per tag, `<number> <what you were doing>`, e.g. `3 fired the pistol at a wall`.
Nothing here is a name by itself; the names go into symbols/manual.sym after somebody looks at the code."""
import sys, re, json, bisect, argparse, collections

ap = argparse.ArgumentParser()
ap.add_argument('record'); ap.add_argument('meta')
ap.add_argument('--sym', action='append', default=[]); ap.add_argument('--tags')
ap.add_argument('--report', default='record_report.md'); ap.add_argument('--emit')
ap.add_argument('--min-hits', type=int, default=3, help='tag-exclusive functions need at least this many hits')
a = ap.parse_args()

entries = sorted(json.load(open(a.meta))['entries'])
def owner(addr):
    i = bisect.bisect_right(entries, addr - 1) - 1
    return entries[i] if i >= 0 else None

names = {}
for f in a.sym:
    for ln in open(f, encoding='utf-8'):
        body, _, note = ln.partition('#'); p = body.split()
        if len(p) >= 3 and p[1] == 'func' and p[2] != '-': names[int(p[0], 16)] = p[2]
def nm(va): return names.get(va) or f'{va:08x}'

labels = {0: 'unlabelled / menus (tag 0)'}
if a.tags:
    for ln in open(a.tags, encoding='utf-8'):
        p = ln.strip().split(None, 1)
        if p and p[0].isdigit(): labels[int(p[0])] = p[1] if len(p) > 1 else ''

# ---- parse the record
fns, apis, tagms, header = {}, [], {}, ''
for ln in open(a.record, encoding='utf-8', errors='replace'):
    ln = ln.rstrip('\n')
    if ln.startswith('# '): header = ln[2:]; continue
    p = ln.split(' ', 1)
    if p[0] == 'tag':
        t, ms = p[1].split(); tagms[int(t)] = int(ms)
    elif p[0] == 'fn':
        main, *rest = ln.split('|')
        w = main.split(); va = int(w[1], 16)
        hits = {int(x.split(':')[0]): int(x.split(':')[1]) for x in w[3:]}
        callers = [int(x, 16) for x in rest[0].split()] if rest else []
        strs = [s.strip() for s in rest[1:] if s.strip()]
        fns[va] = dict(first=int(w[2]), hits=hits, total=sum(hits.values()), callers=callers, strs=strs)
    elif p[0] == 'api':
        main, *rest = ln.split('|')
        w = main.split(); apis.append(dict(site=int(w[1], 16), name=w[2], n=int(w[3]), strs=[s.strip() for s in rest if s.strip()]))

# ---- per-function evidence
fn_apis = collections.defaultdict(list)           # function -> [(api name, count, strings)]
for e in apis:
    o = owner(e['site'])
    if o is not None: fn_apis[o].append(e)
callees = collections.defaultdict(set); callers_of = collections.defaultdict(set)
for va, f in fns.items():
    for ra in f['callers']:
        o = owner(ra)
        if o is not None: callees[o].add(va); callers_of[va].add(o)

CATS = [('gfx', ('IDirect3D', 'd3d9', 'd3dx9', 'ID3DX', 'GDI32', 'gdi32', 'DirectDraw', 'ddraw')),
        ('snd', ('IDirectSound', 'dsound', 'WINMM.dll!wave', 'WINMM.dll!mci', 'mmio')),
        ('input', ('IDirectInput', 'dinput', 'GetAsyncKeyState', 'GetKeyState', 'GetCursorPos', 'ClipCursor', 'SetCursorPos', 'Mouse', 'Joy')),
        ('file', ('CreateFile', 'ReadFile', 'WriteFile', 'fopen', 'fread', 'fwrite', 'fseek', 'FindFirst', 'FindNext', '_open', '_read', 'GetFileAttr', 'SetFilePointer', 'CreateDirectory', 'DeleteFile', 'MoveFile', 'CopyFile', '_stat', '_access', 'remove')),
        ('cfg', ('Profile', 'RegOpen', 'RegQuery', 'RegSet', 'RegCreate', 'getenv', 'GetEnvironment')),
        ('time', ('timeGetTime', 'GetTickCount', 'QueryPerformance', 'Sleep', 'timeSetEvent', 'GetSystemTime', 'GetLocalTime')),
        ('mem', ('HeapAlloc', 'HeapFree', 'VirtualAlloc', 'VirtualFree', 'malloc', 'free', 'HeapReAlloc')),
        ('thread', ('CreateThread', '_beginthread', 'CriticalSection', 'WaitFor', 'CreateEvent', 'SetEvent', 'Mutex', 'Semaphore')),
        ('window', ('CreateWindow', 'ShowWindow', 'DefWindowProc', 'RegisterClass', 'PeekMessage', 'GetMessage', 'DispatchMessage', 'MessageBox', 'SetWindow', 'AdjustWindow', 'GetClientRect', 'GetWindowRect')),
        ('log', ('OutputDebugString', 'fprintf', 'vfprintf', 'printf', 'vsprintf', 'sprintf'))]
def category(apinames):
    c = collections.Counter()
    for n in apinames:
        for cat, keys in CATS:
            if any(k in n for k in keys): c[cat] += 1; break
    return [k for k, _ in c.most_common(2)]

def tagstr(f): return ','.join(f'{t}' for t in sorted(f['hits']))
def one(va):
    f = fns[va]; bits = []
    cats = category([e['name'] for e in fn_apis.get(va, [])])
    if cats: bits.append('cat=' + '+'.join(cats))
    bits.append(f"hits={f['total']}"); bits.append('tags=' + tagstr(f))
    ap_ = [f"{e['name']}x{e['n']}" + (f"({'; '.join(e['strs'][:2])})" if e['strs'] else '') for e in sorted(fn_apis.get(va, []), key=lambda e: -e['n'])[:4]]
    if ap_: bits.append('calls ' + ', '.join(ap_))
    if f['strs']: bits.append('entry-strings ' + ' / '.join(repr(s) for s in f['strs']))
    cn = sorted(callers_of.get(va, ()))[:3]
    if cn: bits.append('from ' + ', '.join(nm(c) for c in cn))
    return '; '.join(bits)

# ---- report
unnamed = [va for va in fns if va not in names]
out = [f'# Play-session report\n\n`{header}`\n\n{len(fns)} engine functions ran ({len(unnamed)} still unnamed); {len(apis)} places in the engine called into the OS / DirectX.\n',
       '## Time per activity tag\n']
for t in sorted(tagms):
    if tagms[t] or t in labels: out.append(f'- tag {t}: {tagms[t] / 1000:.0f} s - {labels.get(t, "(no label)")}')
out.append('')

if len(tagms) > 1:
    for t in sorted(tagms):
        if t == 0 or not tagms[t]: continue
        excl = [va for va, f in fns.items() if set(f['hits']) == {t} and f['hits'][t] >= a.min_hits]
        excl.sort(key=lambda v: -fns[v]['hits'][t])
        out.append(f'## Only while tag {t} was active: {labels.get(t, "")}\n')
        out.append(f'{len(excl)} functions ran only under this tag (at least {a.min_hits} times). Most-called first; those with a name already are marked.\n')
        for va in excl[:60]: out.append(f"- `{va:08x}` {'**' + names[va] + '**' if va in names else 'unnamed'} - {one(va)}")
        out.append('')
else:
    out.append('(only tag 0 was used: no per-activity comparison. Re-record using Ctrl+Alt+F1..F11 to mark what you are doing.)\n')

out.append('## Hottest functions (per whole session)\n')
for va in sorted(fns, key=lambda v: -fns[v]['total'])[:50]: out.append(f"- `{va:08x}` {names.get(va, 'unnamed')} - {one(va)}")
out.append('')

out.append('## Unnamed functions that call the OS or DirectX\n')
cand = [va for va in unnamed if fn_apis.get(va)]
cand.sort(key=lambda v: (category([e['name'] for e in fn_apis[v]]) or ['zz'])[0])
for va in cand[:300]: out.append(f"- `{va:08x}` {one(va)}")
if len(cand) > 300: out.append(f'- ... and {len(cand) - 300} more (see recorded.sym)')
out.append('')

out.append('## Unnamed functions that were handed readable strings\n\nThese are strings found in the registers and first stack arguments on the first calls of a function. They are leads, not proof: a register can still hold a string left over from the caller, so confirm in the code that the function really uses it.\n')
sc = [va for va in unnamed if fns[va]['strs']]
sc.sort(key=lambda v: -fns[v]['total'])
for va in sc[:300]: out.append(f"- `{va:08x}` {one(va)}")
out.append('')

out.append('## Files and configuration the engine touched (every place that opened or probed something)\n')
seen = set()
for e in sorted(apis, key=lambda e: e['site']):
    if not e['strs'] or not category([e['name']]) or category([e['name']])[0] not in ('file', 'cfg'): continue
    key = (owner(e['site']), e['name'], tuple(e['strs']))
    if key in seen: continue
    seen.add(key); out.append(f"- in `{owner(e['site']) or 0:08x}` {nm(owner(e['site']) or 0)}: {e['name']} x{e['n']} {' | '.join(e['strs'])}")
open(a.report, 'w', encoding='utf-8').write('\n'.join(out) + '\n')
print(f'{a.report}: {len(fns)} functions, {len(unnamed)} unnamed, {len(cand)} unnamed with OS/DirectX calls, {len(sc)} unnamed with strings')

if a.emit:
    with open(a.emit, 'w', encoding='utf-8') as o:
        o.write('# darkrecomp recorded evidence (notes only; name "-"), generated by tools/suggest_names.py\n')
        for va in sorted(fns):
            if va in names and not fn_apis.get(va): continue
            o.write(f'0x{va:08x} func - # [rec] {one(va)}\n')
    print(f'{a.emit}: notes for name_sources.py')
