#!/usr/bin/env python3
"""fix_pe.py <exe>: host must load at its fixed base (no ASLR) and run as a GUI program."""
import sys, pefile
p = pefile.PE(sys.argv[1])
p.OPTIONAL_HEADER.DllCharacteristics &= ~(0x40 | 0x20)
p.OPTIONAL_HEADER.Subsystem = 2
p.write(sys.argv[1] + '.new'); p.close()
import os; os.replace(sys.argv[1] + '.new', sys.argv[1])
