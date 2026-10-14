#!/usr/bin/env python3
"""port/tests/fastpaths/check.py <disassembly.txt> [trials]: differential test of port/core/fastpaths.c against the game's own code.

The disassembly is the output of tools/where.py for the functions (as in the dump command in docs; it stays on your
machine, nothing from the game is in the repository). Each instruction is re-assembled at its original address (Keystone)
and run in Unicorn; the same inputs go through the C fast path (built as a shared library). For each function, random
inputs and random hash tables (sizes, probe moduli, slot states, present and absent keys) are tried; every register the
fast path claims to match, the stack pointer, the x87 stack and the memory it writes must be identical. A fast path that
declines (returns 0) is fine; how often it declines is reported. Needs: pip install unicorn keystone-engine; gcc."""
import ctypes, os, random, re, struct, subprocess, sys, tempfile
import keystone, unicorn
from unicorn.x86_const import *

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
dis, trials = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 3000

# ---- the game's code, from the disassembly
ins = {}
for ln in open(dis, errors="replace"):
    m = re.match(r"\s*(?:>>)?\s*([0-9a-f]{8})  (\S+)\s*(.*?)\s*(?:;.*)?$", ln)
    if m and m.group(2) != "int3": ins[int(m.group(1), 16)] = (m.group(2) + " " + m.group(3)).strip()
ks = keystone.Ks(keystone.KS_ARCH_X86, keystone.KS_MODE_32)
code, addrs = {}, sorted(ins)
for k, a in enumerate(addrs):
    text = ins[a].replace("ptr fs:[", "ptr fs:[")
    try: b, _ = ks.asm(text, a)
    except keystone.KsError as e: sys.exit(f"cannot assemble {a:08x} {text}: {e}")
    nxt = addrs[k + 1] if k + 1 < len(addrs) else None
    if nxt is not None and nxt - a < 16 and len(b) > nxt - a: sys.exit(f"{a:08x} {text}: assembled to {len(b)} bytes, the original has {nxt - a}")
    if nxt is not None and nxt - a < 16 and len(b) < nxt - a: b = list(b) + [0x90] * (nxt - a - len(b))   # alignment fillers (lea esp,[esp]) have shorter encodings
    code[a] = bytes(b)

# ---- the fast paths
tmp = tempfile.mkdtemp(); so = os.path.join(tmp, "fp.so")
subprocess.check_call(["gcc", "-O1", "-shared", "-fPIC", "-fno-strict-aliasing", "-fwrapv", "-I", os.path.join(ROOT, "runtime"),
                       os.path.join(ROOT, "port/core/fastpaths.c"), os.path.join(ROOT, "port/tests/fastpaths/shim.c"), "-o", so])
L = ctypes.CDLL(so); L.shim_mem.restype = ctypes.c_void_p; L.shim_cpu_size.restype = ctypes.c_size_t; L.shim_off.restype = ctypes.c_size_t
MEM = L.shim_mem(); CPUSZ = L.shim_cpu_size(); OFF_ST, OFF_TOP, OFF_CW = L.shim_off(1), L.shim_off(2), L.shim_off(3)

STACK, RET, HEAP, TABLE = 0x00200000, 0x00100100, 0x10000000, 0x7df4a8
def new_uc():
    uc = unicorn.Uc(unicorn.UC_ARCH_X86, unicorn.UC_MODE_32)
    for base, size in ((0x400000, 0x800000), (STACK - 0x10000, 0x20000), (HEAP, 0x100000), (0x100000, 0x1000)):
        uc.mem_map(base, size)
    for a, b in code.items(): uc.mem_write(a, b)
    return uc
def put(uc, a, data):                       # same bytes into Unicorn and the C side's guest memory
    uc.mem_write(a, data); ctypes.memmove(MEM + a, data, len(data))

def run_case(fn, setup):
    """setup(put, uc) -> dict(regs, stack args, x87); returns (orig, fast) result tuples, or None if the fast path declined"""
    uc = new_uc(); ctx = setup(lambda a, d: put(uc, a, d))
    regs = {"eax": 0x11111111, "ecx": 0x22222222, "edx": 0x33333333, "ebx": 0x44, "ebp": 0x55, "esi": 0x66, "edi": 0x77}
    regs.update(ctx.get("regs", {}))
    stack = struct.pack("<I", RET) + b"".join(struct.pack("<I", v) for v in ctx.get("args", []))
    esp = STACK; put(uc, esp, stack)
    # ---- original
    for r, u in (("eax", UC_X86_REG_EAX), ("ecx", UC_X86_REG_ECX), ("edx", UC_X86_REG_EDX), ("ebx", UC_X86_REG_EBX), ("ebp", UC_X86_REG_EBP), ("esi", UC_X86_REG_ESI), ("edi", UC_X86_REG_EDI)):
        uc.reg_write(u, regs[r])
    uc.reg_write(UC_X86_REG_ESP, esp)
    x87 = ctx.get("st0")
    if x87 is not None:                      # load st(0) through a tiny stub: fld qword [scratch]; then jump in
        put(uc, 0x100800, struct.pack("<d", x87)); stub = bytes(ks.asm("fld qword ptr [0x100800]; jmp 0x%x" % fn, 0x100000)[0]); uc.mem_write(0x100000, stub)
        start = 0x100000
    else: start = fn
    faulted = False
    try: uc.emu_start(start, RET, count=200000)
    except unicorn.UcError: faulted = True   # e.g. a division by zero: the fast path must decline, so the original faults as before
    o = {r: uc.reg_read(u) for r, u in (("eax", UC_X86_REG_EAX), ("ecx", UC_X86_REG_ECX), ("edx", UC_X86_REG_EDX), ("ebx", UC_X86_REG_EBX), ("ebp", UC_X86_REG_EBP), ("esi", UC_X86_REG_ESI), ("edi", UC_X86_REG_EDI), ("esp", UC_X86_REG_ESP))}
    if x87 is not None: o["fptop"] = (uc.reg_read(UC_X86_REG_FPSW) >> 11) & 7
    o_mem = {a: bytes(uc.mem_read(a, n)) for a, n in ctx.get("watch", [])}
    # ---- fast path
    for a, d in ctx.get("restore", []): ctypes.memmove(MEM + a, d, len(d))   # undo nothing: the C side never ran; the setup put both
    put_c = lambda a, d: ctypes.memmove(MEM + a, d, len(d)); put_c(esp, stack)
    cpu = ctypes.create_string_buffer(CPUSZ); vals = [regs[r] for r in ("eax", "ecx", "edx", "ebx")] + [esp] + [regs[r] for r in ("ebp", "esi", "edi")]
    ctypes.memmove(cpu, struct.pack("<8I", *vals), 32)
    if x87 is not None:
        ctypes.memmove(ctypes.addressof(cpu) + OFF_ST + 8 * 7, struct.pack("<d", x87), 8); ctypes.memmove(ctypes.addressof(cpu) + OFF_TOP, struct.pack("<I", 7), 4)
        ctypes.memmove(ctypes.addressof(cpu) + OFF_CW, struct.pack("<H", 0x27F), 2)
    took = getattr(L, "fp_%08x" % fn)(cpu)
    if faulted: return "faulted-declined" if not took else "faulted-taken"
    if not took: return None
    v = struct.unpack("<8I", cpu.raw[:32]); f = dict(zip(("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"), v))
    if x87 is not None: f["fptop"] = struct.unpack("<I", cpu.raw[OFF_TOP:OFF_TOP + 4])[0]
    f_mem = {a: ctypes.string_at(MEM + a, n) for a, n in ctx.get("watch", [])}
    return o, f, o_mem, f_mem

R = random.Random(1)
perm = list(range(256)); R.shuffle(perm); TBL = struct.pack("<256I", *perm)
def table_case(keysz):
    def setup(put):
        put(TABLE, TBL); put(0xa4ebc8, b"\0\0\0\0")
        size = R.choice([1, 2, 3, 7, 8, 16, 31, 64, 97]); mod = R.choice([1, 2, 3, 5, 8, 13]) if R.random() > 0.03 else 0
        esz = 8 if keysz == 4 else 12; tbl, states, ents, key, out = HEAP, HEAP + 0x100, HEAP + 0x1000, HEAP + 0x8000, HEAP + 0x9000
        fill = R.random(); st = bytes(R.choice([0, 1, 2, 2, 2]) if R.random() < fill else 0 for _ in range(size))
        ent = bytearray(R.getrandbits(8) for _ in range(size * esz))
        k = bytes(R.getrandbits(8) for _ in range(8)) if R.random() < 0.5 else bytes(R.choice([0, 1, 2]) for _ in range(8))
        if R.random() < 0.5 and size:             # plant the key in a used slot somewhere
            i = R.randrange(size); ent[i * esz:i * esz + keysz] = k[:keysz]; st = st[:i] + b"\2" + st[i + 1:]
        put(tbl, struct.pack("<IIIIIII", 0, size, mod, 0, 0, states, ents)); put(states, st); put(ents, bytes(ent)); put(key, k); put(out, b"\xee" * 4)
        if keysz == 4: return {"regs": {"eax": key}, "args": [tbl, out], "watch": [(out, 4)]}
        return {"args": [tbl, key, out], "watch": [(out, 4)]}
    return setup
def hash32_case(put):
    put(TABLE, TBL); return {"regs": {"eax": R.getrandbits(32)}}
def hashn_case(put):
    put(TABLE, TBL); n = R.choice([1, 2, 3, 4, 5, 8, 9, 16, 33]); s = bytes(R.getrandbits(8) for _ in range(n)); put(HEAP, s)
    return {"regs": {"eax": n, "ecx": HEAP}}
def cmp8_case(put):
    a = bytes(R.getrandbits(8) for _ in range(8)); b = bytearray(a)
    if R.random() < 0.5: i = R.randrange(8); b[i] ^= 1 << R.randrange(8)
    put(HEAP, a); put(HEAP + 64, bytes(b)); return {"regs": {"ecx": HEAP, "edx": HEAP + 64}}
def ftol_case(put):
    put(0xa4ebc8, b"\0\0\0\0")
    x = R.choice([R.uniform(-1e6, 1e6), R.uniform(-3, 3), float(R.randint(-2**40, 2**40)) + R.random(), -0.0, 0.5, -0.5, 2**52 + 0.0, float("nan"), 1e300])
    return {"st0": x}

# (function, case, registers the fast path promises to leave exactly as the original)
TESTS = [(0x006a1960, hash32_case, "eax ecx edx"), (0x006a19d0, hashn_case, "eax ecx edx"), (0x0057f640, cmp8_case, "eax edx"),
         (0x00496010, table_case(4), "eax"), (0x00536430, table_case(8), "eax"), (0x006f71d0, ftol_case, "eax edx fptop")]
bad = 0
for fn, case, regs in TESTS:
    if fn not in code: print(f"{fn:08x}: not in the disassembly, skipped"); continue
    n = dec = mism = flt = 0
    for _ in range(trials):
        r = run_case(fn, case); n += 1
        if r is None: dec += 1; continue
        if r == "faulted-declined": dec += 1; flt += 1; continue
        if r == "faulted-taken": mism += 1; print(f"  {fn:08x} MISMATCH: the original faults but the fast path handled the call"); continue
        o, f, om, fm = r
        diffs = [k for k in regs.split() + ["ebx", "ebp", "esi", "edi", "esp"] if o[k] != f[k]] + [f"mem {a:x}" for a in om if om[a] != fm[a]]
        if diffs:
            mism += 1
            if mism <= 3: print(f"  {fn:08x} MISMATCH {diffs}: original {[hex(o[k]) for k in regs.split()]} fast {[hex(f[k]) for k in regs.split()]}")
    bad += mism
    print(f"{fn:08x}: {n} cases, {mism} mismatches, fast path declined {dec} (of which the original faults: {flt})")
print("fastpaths: " + ("all match" if not bad else f"{bad} MISMATCHES"))
sys.exit(1 if bad else 0)
