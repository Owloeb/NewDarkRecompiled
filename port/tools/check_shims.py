#!/usr/bin/env python3
"""check_shims.py [mingw lib32 dir]: cross-checks every shim table in port/ against the real Windows ABI.

For each shim whose name is a stdcall export of a system DLL, the number of argument bytes the shim pops must equal the
@N decoration in MinGW's import definitions (lib32/*.def, shipped with Zig: `pip install ziglang`). A wrong pop count
shifts the guest stack and usually surfaces much later as a stack-cookie failure, so this is checked statically.
Exit status 1 if anything disagrees."""
import os, re, sys, glob
HERE = os.path.dirname(os.path.abspath(__file__)); PORT = os.path.dirname(HERE)
def mingw_dir():
    if len(sys.argv) > 1: return sys.argv[1]
    import ziglang; return os.path.join(os.path.dirname(ziglang.__file__), "lib", "libc", "mingw", "lib32")
DLLS = ["kernel32", "user32", "gdi32", "advapi32", "winmm", "version", "dsound", "dinput", "dinput8", "d3d9", "shell32", "ole32", "ddraw", "d3dx9_43", "wsock32", "ws2_32"]
abi = {}
for d in DLLS:
    p = os.path.join(mingw_dir(), d + ".def")
    if not os.path.exists(p): continue
    for line in open(p, errors="replace"):
        m = re.match(r"\s*([A-Za-z_][\w]*)@(\d+)\b", line)
        if m: abi.setdefault(m.group(1), (int(m.group(2)), d))
# shim tables: S(name, pop)  SA("name", impl, pop)  { "name", sh_x, pop }
def pop_bytes(expr):
    expr = expr.strip()
    if expr in ("CDECL", "0"): return 0
    m = re.fullmatch(r"STD\((\d+)\)", expr)
    if m: return 4 * int(m.group(1))
    if expr.isdigit(): return int(expr)
    return None
shims = []
for f in sorted(glob.glob(os.path.join(PORT, "*.c")) + glob.glob(os.path.join(PORT, "*", "*.c"))):
    src = open(f).read()
    for m in re.finditer(r"\bS\(\s*(\w+)\s*,\s*([^)]*\)?)\s*\)", src): shims.append((m.group(1), m.group(2), f))
    for m in re.finditer(r'\bSA\(\s*"([^"]+)"\s*,\s*\w+\s*,\s*([^)]*\)?)\s*\)', src): shims.append((m.group(1), m.group(2), f))
    for m in re.finditer(r'\{\s*"([^"]+)"\s*,\s*sh_\w+\s*,\s*([^}]*?)\s*\}', src): shims.append((m.group(1), m.group(2), f))
bad = 0; checked = 0
for name, expr, f in shims:
    pb = pop_bytes(expr)
    if pb is None: continue
    key = name[:-1] if name.endswith("A") and name not in abi and name[:-1] in abi else name
    if key in abi:
        checked += 1; want, dll = abi[key]
        if pb != want:
            bad += 1; print(f"{os.path.relpath(f, PORT)}: {name} pops {pb} bytes, {dll}.dll's {name} takes {want}")
print(f"{checked} stdcall shims checked against MinGW import definitions, {bad} wrong")
sys.exit(1 if bad else 0)
