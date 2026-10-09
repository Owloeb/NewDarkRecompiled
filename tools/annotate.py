#!/usr/bin/env python3
"""annotate.py <SS2.exe> <lifted dir, e.g. out/nd> <out.sym>

Harvests names and notes for the recompiled engine from the executable itself (nothing else is used):

  * MSVC RTTI: every polymorphic class has a type descriptor (its decorated name), a complete object locator and a
    vtable. Each vtable slot is a virtual method, named Class::vfN after the base-most class that has it in that slot.
    Classes deriving from IUnknown get QueryInterface/AddRef/Release for slots 0-2.
  * Constructors/destructors: functions that store a class's vtable pointer into an object.
  * Import thunks: functions that are only `jmp [import]`.
  * Console commands: the static command table (name, argument type, handler, help text); handlers become cmd_<name>.
  * String references: the literals a function uses (log messages, config variables, ...) as notes.

Output (one line per item, addresses as in the original exe):
    0x00601430 func Name                 # note
    0x00787820 vtbl Class                # 41 slots
Hand-written names can be added to a separate file and take precedence (see README).
"""
import sys, re, struct, bisect, zlib, collections
import pefile, capstone

exe, lifted, out = sys.argv[1:4]
pe = pefile.PE(exe); base = pe.OPTIONAL_HEADER.ImageBase; img = pe.get_memory_mapped_image()
raw = open(exe, 'rb').read(); crc = zlib.crc32(raw) & 0xffffffff
secs = [(s.Name.rstrip(b'\0').decode(), base + s.VirtualAddress, base + s.VirtualAddress + s.Misc_VirtualSize, s.Characteristics) for s in pe.sections]
code = [(lo, hi) for n, lo, hi, c in secs if c & 0x20000000]
data = [(lo, hi) for n, lo, hi, c in secs if not c & 0x20000000]
def u32(va): return struct.unpack_from('<I', img, va - base)[0]
def in_img(va): return base <= va < base + len(img)
def is_code(va): return any(lo <= va < hi for lo, hi in code)
def is_data(va): return any(lo <= va < hi for lo, hi in data)

# ---- function list from the lifted table
funcs = sorted(int(m, 16) for m in re.findall(r'\{0x([0-9a-f]{8})u,', open(f'{lifted}/nd_table.c').read()))
fset = set(funcs)
def owner(va):
    i = bisect.bisect_right(funcs, va) - 1
    return funcs[i] if i >= 0 else None

# ---- MSVC RTTI
_PRIM = {'C': 'schar', 'D': 'char', 'E': 'uchar', 'F': 'short', 'G': 'ushort', 'H': 'int', 'I': 'uint', 'J': 'long',
         'K': 'ulong', 'M': 'float', 'N': 'double', 'X': 'void'}
def _name(s, i, backrefs):
    """one name segment: plain 'id@', a template '?$id@args@', or a back-reference digit"""
    if s[i].isdigit():
        return backrefs[int(s[i])] if int(s[i]) < len(backrefs) else '?', i + 1
    if s.startswith('?$', i):
        j = s.index('@', i + 2); t = s[i + 2:j]; i = j + 1; args = []; inner = [t]
        while s[i] != '@':
            a, i = _arg(s, i, inner)
            if a: args.append(a)
        return f'{t}<{",".join(args)}>', i + 1
    j = s.index('@', i); n = s[i:j]; backrefs.append(n); return n, j + 1
def _qual(s, i, backrefs):
    segs = []
    while s[i] != '@':
        n, i = _name(s, i, backrefs); segs.append(n)
    return '::'.join(reversed(segs)), i + 1
def _arg(s, i, backrefs):
    c = s[i]
    if c in 'VUW':
        if c == 'W': i += 1          # enum: W4name@@
        return _qual(s, i + 1, backrefs)
    if s.startswith('_N', i): return 'bool', i + 2
    if c in _PRIM: return _PRIM[c], i + 1
    if c in 'PQ' and i + 1 < len(s) and s[i + 1] in 'ABCD':
        a, i = _arg(s, i + 2, backrefs); return (a or '?') + '*', i
    if s.startswith('$0', i):            # integer constant
        if s[i + 2] in '0123456789': return s[i + 2], i + 3
        j = s.index('@', i + 2); return None, j + 1
    if s.startswith('$1?', i):           # address of a symbol: $1?name@@3<type><cv>
        j = s.index('@@', i + 3); sym = s[i + 3:j]; k = j + 2
        if s[k] == '3':
            _, k = _arg(s, k + 1, backrefs)
            if k < len(s) and s[k] in 'ABCD': k += 1
        return '&' + sym, k
    raise ValueError(s[i:])
def demangle(td_name):
    """.?AVcFoo@@ -> cFoo ; .?AVInner@Outer@@ -> Outer::Inner ; .?AV?$cProperty@UIFoo@@$1?IID_IFoo@@3U_GUID@@B@@ -> cProperty<IFoo,&IID_IFoo>"""
    s = td_name[4:]
    try:
        n, i = _qual(s, 0, [])
        return n
    except Exception:
        return s.rstrip('@').replace('@', '_').replace('?', '').replace('$', '')

tds = {}
for m in re.finditer(rb'\.\?A[VU][^\x00]{1,400}\x00', img):
    tds[base + m.start() - 8] = m.group(0)[:-1].decode('latin1')
cols = {}
for off in range(0, len(img) - 20, 4):
    sig, o, cd, ptd, pch = struct.unpack_from('<5I', img, off)
    if sig == 0 and ptd in tds and in_img(pch): cols[base + off] = (ptd, o, cd, pch)

def bases_of(pch):
    try:
        _, attr, n, pbca = struct.unpack_from('<4I', img, pch - base)
        names = []
        for k in range(min(n, 64)):
            bcd = u32(pbca + 4 * k)
            names.append(demangle(tds.get(u32(bcd), '.?AV?@@')))
        return names
    except Exception:
        return []

vts = {}   # vtable va -> (class, offset, [slots], bases)
colset = set(cols)
for off in range(0, len(img) - 8, 4):
    p = struct.unpack_from('<I', img, off)[0]
    if p in colset:
        vt = base + off + 4; slots = []
        while in_img(vt + 4 * len(slots)) and is_code(u32(vt + 4 * len(slots))):
            slots.append(u32(vt + 4 * len(slots)))
        if slots:
            ptd, o, cd, pch = cols[p]
            vts[vt] = (demangle(tds[ptd]), o, slots, bases_of(pch))

names, notes = {}, collections.defaultdict(list)
cand = collections.defaultdict(list)   # func -> [(nslots, secondary, class, slot)]
shared_slots = collections.defaultdict(set)
for vt, (cls, o, slots, bases) in vts.items():
    com = 'IUnknown' in bases
    for i, f in enumerate(slots):
        shared_slots[f].add(i)
        meth = ('QueryInterface', 'AddRef', 'Release')[i] if com and i < 3 else f'vf{i}'
        cand[f].append((len(slots), o != 0, cls, meth))
for f, cs in cand.items():
    if f not in fset: continue
    if len(shared_slots[f]) > 3:   # same body in many different slots: a generic stub (purecall, empty method, ...)
        names[f] = f'shared_virtual_{f:08x}'; notes[f].append(f'used in {len(cs)} vtable slots'); continue
    cs.sort()
    n, sec, cls, meth = cs[0]
    names[f] = f'{cls}::{meth}'
    if len(cs) > 1: notes[f].append(f'also in {len(cs) - 1} other vtable slot(s)')

# ---- disassembly pass: ctor/dtor (stores of a vtable address), import thunks, string references
iat = {}
for d in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
    for i in d.imports:
        if i.name: iat[i.address] = i.name.decode()
vtaddr = set(vts)
def cstring(va):
    if not is_data(va): return None
    o = va - base; e = img.find(b'\0', o, o + 200)
    if e < 0 or e - o < 5: return None
    s = img[o:e]
    if not all(32 <= b < 127 or b in (9, 10, 13) for b in s): return None
    if sum(chr(b).isalpha() for b in s) < 4: return None
    return s.decode('latin1').replace('\n', '\\n').replace('\r', '').replace('\t', ' ')

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.detail = True
ctors = collections.defaultdict(set); strs = collections.defaultdict(list); thunk = {}
code_end = {lo: hi for lo, hi in code}
for k, f in enumerate(funcs):
    end = funcs[k + 1] if k + 1 < len(funcs) else f + 0x1000
    seg_hi = next((hi for lo, hi in code if lo <= f < hi), None)
    if seg_hi is None: continue
    end = min(end, seg_hi, f + 0x4000)
    first = True
    for ins in md.disasm(img[f - base:end - base], f):
        ops = ins.operands
        if first:
            first = False
            if ins.mnemonic == 'jmp' and ops and ops[0].type == capstone.x86.X86_OP_MEM and ops[0].mem.base == 0 and (ops[0].mem.disp & 0xffffffff) in iat:
                thunk[f] = iat[ops[0].mem.disp & 0xffffffff]; break
        for op in ops:
            v = None
            if op.type == capstone.x86.X86_OP_IMM: v = op.imm & 0xffffffff
            elif op.type == capstone.x86.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0: v = op.mem.disp & 0xffffffff
            if v is None: continue
            if v in vtaddr and ins.mnemonic == 'mov' and ops[0].type == capstone.x86.X86_OP_MEM: ctors[f].add(v)
            elif ins.mnemonic in ('push', 'mov', 'lea'):
                s = cstring(v)
                if s and s not in strs[f] and len(strs[f]) < 3: strs[f].append(s)
        if ins.mnemonic in ('ret', 'int3'): pass

for f, lib in thunk.items():
    names.setdefault(f, f'imp_{lib}')
for f, vs in ctors.items():
    if f in names: continue
    cls = sorted({vts[v][0] for v in vs})
    names[f] = f'{cls[0]}::ctor_or_dtor' if len(cls) == 1 else f'{cls[-1]}::ctor_or_dtor'
    if len(cls) > 1: notes[f].append('sets vtables of ' + ', '.join(cls[:4]))
# ---- console commands: a static table of {name, argument type, handler, help text, 0, 0} (24-byte entries) in the data
# section. The handler becomes cmd_<name>; a name found some other way (RTTI) is kept and the command goes into the note.
def _cstr(va, n=96):
    if not in_img(va): return None
    d = img[va - base: va - base + n].split(b'\0')[0]
    return d.decode() if 2 <= len(d) < 64 and all(32 <= c < 127 for c in d) else None
_ARG = {0: 'no argument', 2: 'int argument', 3: 'float argument', 5: 'string argument'}
cmds = {}
for lo, hi in data:
    for a in range(lo, hi - 24, 4):
        nm_, typ, h, hlp, z1, z2 = struct.unpack_from('<6I', img, a - base)
        if h in fset and z1 in (0, 0xffffffff) and z2 == 0 and typ in _ARG and nm_ >= base:
            n = _cstr(nm_)
            if n and re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', n) and (hlp == 0 or _cstr(hlp)):
                cmds.setdefault(h, (n, _cstr(hlp) if hlp else '', typ))
for h, (n, hlp, typ) in cmds.items():
    if h not in names: names[h] = f'cmd_{n}'
    notes[h].append(f'console command "{n}" ({_ARG[typ]})' + (f': {hlp}' if hlp else ''))

for f, ss in strs.items():
    notes[f].append('refs ' + ' | '.join(repr(s[:70]) for s in ss))

# names must be unique: suffix duplicates with the address
seen = collections.Counter(names.values())
for f in list(names):
    if seen[names[f]] > 1: names[f] += f'_{f:08x}'

with open(out, 'w') as fo:
    fo.write(f'# darkrecomp symbols for NewDark 2.48 SS2.exe (crc32 {crc:08x}), generated by tools/annotate.py\n')
    fo.write('# <address> <kind> <name>  # notes.   kinds: func, vtbl\n')
    for vt in sorted(vts):
        cls, o, slots, bases = vts[vt]
        fo.write(f'0x{vt:08x} vtbl {cls}{"@" + str(o) if o else ""}  # {len(slots)} slots' + (f'; bases: {", ".join(bases[1:6])}' if len(bases) > 1 else '') + '\n')
    for f in funcs:
        if f in names or f in notes:
            nm = names.get(f, '-')
            fo.write(f'0x{f:08x} func {nm}' + (f'  # {"; ".join(notes[f])}' if notes[f] else '') + '\n')
named = sum(1 for f in funcs if f in names)
print(f'{len(funcs)} functions: {named} named ({100 * named / len(funcs):.1f}%), {sum(1 for f in funcs if f not in names and f in notes)} more with notes; '
      f'{len(vts)} vtables; {len(thunk)} import thunks; {len(ctors)} ctor/dtor candidates; {len(cmds)} console commands')
