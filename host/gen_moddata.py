#!/usr/bin/env python3
"""gen_moddata.py <module.osm|dll> <prefix> <out.c> [--rebase 0xADDR]

Describes a recompiled DLL for the host (runtime/recomp_mod.h): where its code expects to live, its entry point, its
code ranges, and a CRC of the code bytes as mapped there, so the host only swaps in the recompiled code when the file
on disk is the one that was lifted.

Without --rebase the module must load at its preferred base (Windows maps it). With --rebase it was lifted as if
loaded at that address, and the host maps and relocates the file there itself (needed when the preferred base is
taken, e.g. by another script module, or when the DLL asks for address randomisation)."""
import sys, zlib, argparse, pefile
ap = argparse.ArgumentParser()
ap.add_argument('dll'); ap.add_argument('prefix'); ap.add_argument('out')
ap.add_argument('--rebase', type=lambda v: int(v, 0))
a = ap.parse_args()
pe = pefile.PE(a.dll)
file_base = pe.OPTIONAL_HEADER.ImageBase
manual = a.rebase is not None
if manual and a.rebase != file_base: pe.relocate_image(a.rebase)
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
code = [(s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize) for s in pe.sections if s.Characteristics & 0x20000000]
crc = 0
for lo, hi in code: crc = zlib.crc32(img[lo:hi], crc)
name = a.dll.replace('\\', '/').split('/')[-1]
p = a.prefix
L = ['#include "recomp_mod.h"',
     f'extern const rc_ent {p}_table[];', f'extern const unsigned {p}_table_n;', f'guest_fn {p}_lookup(uint32_t va);',
     f'static const uint32_t code_lo[] = {{ {", ".join(f"0x{base + x:08x}u" for x, _ in code)} }};',
     f'static const uint32_t code_hi[] = {{ {", ".join(f"0x{base + y:08x}u" for _, y in code)} }};',
     f'const RecompModDesc {p}_desc = {{ "{name}", 0x{base:08x}u, 0x{pe.OPTIONAL_HEADER.SizeOfImage:x}u, '
     f'0x{base + pe.OPTIONAL_HEADER.AddressOfEntryPoint:08x}u, 0x{file_base:08x}u, 0x{crc & 0xffffffff:08x}u, {int(manual)}, '
     f'{len(code)}, code_lo, code_hi, {p}_table, &{p}_table_n, {p}_lookup }};']
open(a.out, 'w').write('\n'.join(L) + '\n')
print(f'{name}: base {base:#x}{" (rebased from %#x)" % file_base if manual else ""}, {len(code)} code ranges, crc {crc & 0xffffffff:08x}')
