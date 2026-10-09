#!/usr/bin/env python3
"""fnprof.py <exe> <ss2port.log> [skip] [top]: summarise the PROF tables of a PORT_PROF build and say what the hottest guest functions look like.
    python3 tools/fnprof.py SS2.exe ss2port.log 4 25
skip = how many 20 s tables to ignore at the start (loading); the rest are averaged. Only reads the exe; prints no game code beyond
mnemonic counts and the first few instructions of each function."""
import sys, re, collections, pefile, capstone
exe, log = sys.argv[1], sys.argv[2]; skip = int(sys.argv[3]) if len(sys.argv) > 3 else 4; top = int(sys.argv[4]) if len(sys.argv) > 4 else 25
tabs, cur = [], None
for l in open(log, errors="replace"):
    if "top guest functions" in l: cur = collections.OrderedDict(); tabs.append(cur)
    m = re.search(r"PROF\s+([\d.]+)% (\S+)", l)
    if m and cur is not None: cur[m.group(2)] = float(m.group(1))
use = tabs[skip:] or tabs; tot = collections.Counter()
for t in use:
    for k, v in t.items(): tot[k] += v / len(use)
pe = pefile.PE(exe); base = pe.OPTIONAL_HEADER.ImageBase
imps = {base + i.address - base: d.dll.decode() + "!" + (i.name.decode() if i.name else "#%d" % i.ordinal) for d in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []) for i in d.imports}
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.detail = True
print(f"{len(tabs)} tables, averaging {len(use)}; top {top}\n")
cum = 0
for name, pct in tot.most_common(top):
    cum += pct
    m = re.match(r"(\w\w)_([0-9a-f]{8})$", name)
    if not m or m.group(1) != "nd": print(f"{pct:5.1f}% (cum {cum:4.1f}%) {name}  (not in SS2.exe)\n"); continue
    va = int(m.group(2), 16); rva = va - base
    sec = next((s for s in pe.sections if s.VirtualAddress <= rva < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData)), None)
    data = pe.get_data(rva, 2048); ins = []; seen_ret = False
    for i in md.disasm(data, va):
        if seen_ret and i.mnemonic in ("int3", "nop") : break
        ins.append(i); seen_ret = i.mnemonic in ("ret", "jmp") and i.mnemonic == "ret"
        if len(ins) > 400: break
    mn = collections.Counter(i.mnemonic for i in ins)
    fp = sum(v for k, v in mn.items() if k.startswith("f")); rep = sum(v for k, v in mn.items() if k.startswith("rep") or k in ("movsd", "stosd", "movsb", "stosb", "scasb", "cmpsb", "lodsb"))
    calls = [i for i in ins if i.mnemonic == "call"]; loops = sum(1 for i in ins if i.mnemonic.startswith("j") and i.operands and i.operands[0].type == capstone.x86.X86_OP_IMM and i.operands[0].imm <= i.address)
    names = collections.Counter()
    for c in calls:
        o = c.operands[0]
        names[("imp " + imps.get(o.mem.disp, "?")) if o.type == capstone.x86.X86_OP_MEM and o.mem.base == 0 and o.mem.index == 0 else ("call 0x%x" % o.imm if o.type == capstone.x86.X86_OP_IMM else "indirect")] += 1
    print(f"{pct:5.1f}% (cum {cum:4.1f}%) {name}  ~{ins[-1].address + ins[-1].size - va if ins else 0} bytes, {len(ins)} insns, fp {fp}, string-ops {rep}, backward jumps {loops}, imul/div {mn['imul'] + mn['idiv'] + mn['div'] + mn['mul']}")
    print("   calls:", ", ".join(f"{k} x{v}" for k, v in names.most_common(6)) or "none")
    print("   head: ", " | ".join(f"{i.mnemonic} {i.op_str}" for i in ins[:8]), "\n")
