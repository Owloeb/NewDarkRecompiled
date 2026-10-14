#!/usr/bin/env python3
"""Regression: lift_x87's analysis must cope with a branch into bytes that don't decode (allobjs.osm has such junk; it
crashed the lifter with 'NoneType' object has no attribute 'mnemonic'). Run from the repository root."""
import os, sys
sys.path.insert(0, os.getcwd())
from lift import SynthImage, Lifter, Fn
blob = bytes.fromhex('d9e8' '7403' 'ddd8' 'c3' '0f04' '9090')   # fld1; jz bad; fstp st(0); ret; bad: 0f 04 (undecodable)
base = 0x401000
img = SynthImage(base, blob, [base])
assert img.decode(base + 7) is None, 'test needs an undecodable instruction'
L = Lifter(None, 'sx', img); L.cache_regs = True
L.flags_analyze(); L.regs_analyze(); L.x87_analyze()
f = Fn(L, base); f.walk(); code = f.emit()
assert 'x87tb' in code, 'the function should still get x87 locals'
print('x87_undecodable: ok')
