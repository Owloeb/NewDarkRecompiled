#!/usr/bin/env python3
"""check_com.py [mingw include dir]: verifies port/dx/ifaces.h (the COM vtable layouts the DirectX front ends build)
against MinGW's DirectX headers. Slot order and argument counts must match exactly: a method in the wrong slot, or
with the wrong count, pops the wrong number of bytes from the guest stack. Exit status 1 on any difference."""
import os, re, sys
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import gen_ifaces
decl = gen_ifaces.parse(gen_ifaces.inc_dir())
have = dict(re.findall(r'#define IFACE_(\w+) "([^"]*)"', open(os.path.join(os.path.dirname(HERE), "dx", "ifaces.h")).read()))
bad = 0
for name in gen_ifaces.IFACES:
    want = " ".join(f"{m}:{a}" for m, a in gen_ifaces.full(decl, name))
    if have.get(name) != want:
        bad += 1; print(f"{name}: ifaces.h differs from the headers (regenerate with tools/gen_ifaces.py)")
print(f"{len(gen_ifaces.IFACES)} interface layouts checked, {bad} differ")
sys.exit(1 if bad else 0)
