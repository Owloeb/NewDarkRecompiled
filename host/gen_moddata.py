#!/usr/bin/env python3
"""gen_moddata.py <module.osm|dll> <prefix> <out.c>
Describes a recompiled DLL for the host: preferred base, size, entry point, code ranges, and a CRC of the code bytes
(as mapped at the preferred base) so the host only swaps in the recompiled code when the file on disk is the one that
was lifted."""
import sys, zlib, pefile
path, prefix, out = sys.argv[1:4]
pe = pefile.PE(path)
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
code = [(s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize) for s in pe.sections if s.Characteristics & 0x20000000]
crc = 0
for lo, hi in code: crc = zlib.crc32(img[lo:hi], crc)
name = path.replace('\\', '/').split('/')[-1]
L = [f'#include <stdint.h>',
     f'const char {prefix}_mod_name[] = "{name}";',
     f'const uint32_t {prefix}_mod_base = 0x{base:08x}u, {prefix}_mod_size = 0x{pe.OPTIONAL_HEADER.SizeOfImage:x}u;',
     f'const uint32_t {prefix}_mod_entry = 0x{base + pe.OPTIONAL_HEADER.AddressOfEntryPoint:08x}u, {prefix}_mod_crc = 0x{crc & 0xffffffff:08x}u;',
     f'const unsigned {prefix}_mod_ncode = {len(code)};',
     f'const uint32_t {prefix}_mod_code_lo[] = {{ {", ".join(f"0x{base + a:08x}u" for a, _ in code)} }};',
     f'const uint32_t {prefix}_mod_code_hi[] = {{ {", ".join(f"0x{base + b:08x}u" for _, b in code)} }};']
open(out, 'w').write('\n'.join(L) + '\n')
print(f'{name}: base {base:#x}, {len(code)} code ranges, crc {crc & 0xffffffff:08x}')
