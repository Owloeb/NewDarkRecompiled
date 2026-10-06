#!/usr/bin/env python3
"""name_sources.py <lifted dir, e.g. out/nd> <out dir> <file.sym>...
Writes a copy of the generated C with each function preceded by its name and notes from the symbol files
(later files win), plus functions.txt, an address -> name index. The copy is for reading only; the build uses the
original files."""
import sys, os, re, glob
src, dst, files = sys.argv[1], sys.argv[2], sys.argv[3:]
names, notes = {}, {}
for f in files:
    if not os.path.exists(f): continue
    for ln in open(f, encoding='utf-8'):
        body, _, note = ln.partition('#')
        p = body.split()
        if len(p) >= 3 and p[1] == 'func':
            va = int(p[0], 16)
            if p[2] != '-': names[va] = p[2]
            if note.strip(): notes[va] = note.strip()
os.makedirs(dst, exist_ok=True)
for fn in sorted(glob.glob(os.path.join(src, '*.c'))) + sorted(glob.glob(os.path.join(src, '*.h'))):
    out = []
    for ln in open(fn, encoding='utf-8', errors='replace'):
        m = re.match(r'void [a-z]+_([0-9a-f]{8})\(CPU \*c\) \{', ln)
        if m:
            va = int(m.group(1), 16)
            if va in names or va in notes:
                out.append(f'/* {names.get(va, "")}{"  --  " + notes[va] if va in notes else ""} */\n')
        out.append(ln)
    open(os.path.join(dst, os.path.basename(fn)), 'w', encoding='utf-8').writelines(out)
with open(os.path.join(dst, 'functions.txt'), 'w', encoding='utf-8') as fo:
    for va in sorted(set(names) | set(notes)):
        fo.write(f'{va:08x}  {names.get(va, "-"):60s} {notes.get(va, "")}\n')
print(f'{len(names)} names, {len(notes)} notes -> {dst}')
