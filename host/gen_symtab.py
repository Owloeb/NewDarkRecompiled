#!/usr/bin/env python3
"""gen_symtab.py [--functions nd_meta.json] <SS2.exe> <out.c> <file.sym>...
Builds the name table the host uses in crash reports. Later files override earlier ones (pass symbols/manual.sym last).
Files whose header names a different exe (crc32) are skipped.
--functions: also list every other function entry with no name (name NULL), so a host can find the function that contains an
address (the portable host does; the Windows host only looks up exact entry addresses)."""
import sys, re, zlib, json
argv = sys.argv[1:]; allfn = None
if argv and argv[0] == '--functions': allfn = json.load(open(argv[1]))['entries']; argv = argv[2:]
exe, out, files = argv[0], argv[1], argv[2:]
crc = zlib.crc32(open(exe, 'rb').read()) & 0xffffffff
names = {}
for f in files:
    try: lines = open(f, encoding='utf-8').read().splitlines()
    except FileNotFoundError: continue
    m = re.search(r'crc32 ([0-9a-f]{8})', lines[0] if lines else '')
    if m and int(m.group(1), 16) != crc:
        print(f'  {f}: made for a different SS2.exe, skipped'); continue
    n = 0
    for ln in lines:
        ln = ln.split('#')[0].split()
        if len(ln) >= 3 and ln[1] == 'func' and ln[2] != '-':
            names[int(ln[0], 16)] = ln[2]; n += 1
    print(f'  {f}: {n} names')
def cstr(s): return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'
L = ['#include <stdint.h>', 'typedef struct { uint32_t va; const char *name; } SymEnt;', 'const SymEnt nd_symtab[] = {']
entries = {va: cstr(n) for va, n in names.items()}
for va in (allfn or []): entries.setdefault(va, '0')
L += [f'    {{0x{va:08x}u, {n}}},' for va, n in sorted(entries.items())]
L += ['    {0xffffffffu, 0}', '};', f'const unsigned nd_symtab_n = {len(entries)};']
open(out, 'w').write('\n'.join(L) + '\n')
