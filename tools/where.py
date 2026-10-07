#!/usr/bin/env python3
"""where.py <exe> <va> [before] [after]: disassemble the code around a guest address (e.g. a return address from a crash report).
    python3 tools/where.py "SS2.exe" 0x4aff46          # 96 bytes before, 16 after; '>>' marks the address"""
import sys, pefile, capstone
exe, va = sys.argv[1], int(sys.argv[2], 16)
before = int(sys.argv[3]) if len(sys.argv) > 3 else 96; after = int(sys.argv[4]) if len(sys.argv) > 4 else 16
pe = pefile.PE(exe); base = pe.OPTIONAL_HEADER.ImageBase
rva = va - base
sec = next((s for s in pe.sections if s.VirtualAddress <= rva < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData)), None)
if not sec: sys.exit("address is not inside a section of this exe")
lo = max(rva - before, sec.VirtualAddress); data = pe.get_data(lo, rva + after - lo)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
# resolve imports so calls through the IAT show names
imps = {base + i.address - base: (d.dll.decode() + "!" + (i.name.decode() if i.name else "#%d" % i.ordinal)) for d in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []) for i in d.imports}
# disassembly from an arbitrary byte can start mid-instruction: try a few starts and keep the one that lands on the address
best = None
for skip in range(0, 12):
    ins = list(md.disasm(data[skip:], base + lo + skip))
    if any(i.address == va for i in ins): best = ins; break
for i in best or list(md.disasm(data, base + lo)):
    note = ""
    for tok in i.op_str.replace("[", " ").replace("]", " ").split():
        if tok.startswith("0x") and int(tok, 16) in imps: note = "   ; " + imps[int(tok, 16)]
    print(("%s %08x  %-8s %s%s" % (">>" if i.address == va else "  ", i.address, i.mnemonic, i.op_str, note)))
