#!/usr/bin/env python3
"""port/build.py: build the portable host (ss2port) from your own SS2.exe (NewDark 2.48). Linux / WSL / macOS.

    python3 port/build.py "/path/to/System Shock 2/SS2.exe" [--backend sdl2|null] [--jobs N] [--target linux|windows] [--install]
                          [--cache-regs]

--cache-regs lifts with lift.py --cache-regs (guest registers in C locals; experimental, see runtime/rt_fast.h). The lifted
code then differs from the default, so switching it on or off relifts everything.

--target windows cross-compiles a 64-bit Windows ss2port.exe (with SDL2.dll) using Zig: python3 -m pip install ziglang.
--install copies the result next to SS2.exe.

Steps: (1) lift SS2.exe and its DLLs (allobjs.osm, Squirrel.osm, lgvid.dll) to C: about 8 minutes the first time, skipped
when nothing changed, (2) build the host with CMake. The result is build/port/ss2port; run it from the game folder.
Needs: python3 -m pip install pefile capstone, cmake, ninja or make, gcc or clang, and libsdl2-dev for --backend sdl2."""
import argparse, glob, hashlib, os, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))); PY = sys.executable
OUT = os.path.join(ROOT, "out", "port")                 # kept apart from the Windows build's out/nd, out/ao, ...
def find_ci(base, *parts):
    """base/parts..., matching each part case-insensitively (the game looks files up that way; Linux file systems don't)"""
    p = base
    for part in parts:
        if part in (".", ""): continue
        q = os.path.join(p, part)
        if not os.path.exists(q) and os.path.isdir(p):
            q = next((os.path.join(p, e) for e in sorted(os.listdir(p)) if e.lower() == part.lower()), q)
        p = q
    return p
MODULES = [("allobjs.osm", "Data", "ao", None), ("Squirrel.osm", "osm", "sq", 0x30000000), ("lgvid.dll", ".", "lv", 0x30300000)]
MODULES_LOW = [("allobjs.osm", "Data", "ao", 0x00E00000), ("Squirrel.osm", "osm", "sq", 0x00E90000), ("lgvid.dll", ".", "lv", 0x01130000)]   # --low-dll-bases: for hosts with a small guest space (PS Vita)

def run(cmd, what):
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode: sys.exit(f"\nFAILED: {what}\n  {' '.join(cmd)}\n{r.stdout[-3000:]}{r.stderr[-3000:]}")

def sha(*paths, extra=""):
    h = hashlib.sha1(extra.encode())
    for p in paths: h.update(open(p, "rb").read())
    return h.hexdigest()

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ss2exe"); ap.add_argument("--backend", default="sdl2", choices=["sdl2", "null"])
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2); ap.add_argument("--build")
    ap.add_argument("--target", default="linux", choices=["linux", "windows"], help="linux: this machine (any POSIX system); windows: cross-compile a Windows exe")
    ap.add_argument("--install", action="store_true", help="copy the result next to SS2.exe")
    ap.add_argument("--cache-regs", action="store_true", help="lift with lift.py --cache-regs (experimental)")
    ap.add_argument("--symbols", action="store_true", help="embed a function-name table (tools/annotate.py + symbols/manual.sym) so crash reports show names instead of addresses; adds a few hundred KB, so it is off by default")
    ap.add_argument("--low-dll-bases", action="store_true", help="lift the game's DLLs for load addresses below 32 MB (hosts with a small guest space, e.g. the PS Vita backend)")
    a = ap.parse_args(); exe = os.path.abspath(a.ss2exe)
    win = a.target == "windows"
    if os.path.isfile(exe): sys.path.insert(0, os.path.join(ROOT, "tools")); import exe_stamp; exe_stamp.check(exe)
    if not a.build: a.build = os.path.join(ROOT, "build", "port-win" if win else "port")
    if win:
        try: import ziglang  # noqa: F401
        except ImportError: sys.exit("--target windows needs Zig: python3 -m pip install ziglang")
    if not os.path.isfile(exe): sys.exit(f"SS2.exe not found: {exe}")
    for m in ("pefile", "capstone"):
        try: __import__(m)
        except ImportError: sys.exit(f"missing Python package '{m}': python3 -m pip install pefile capstone")
    for t in ("cmake",):
        if not shutil.which(t): sys.exit(f"missing tool '{t}'")
    if not win and not (shutil.which("gcc") or shutil.which("clang") or shutil.which("cc")): sys.exit("missing C compiler (gcc or clang)")

    print("[1/2] lifting (about 8 minutes the first time)", flush=True)
    cr = ["--cache-regs"] if a.cache_regs else []; crs = "cache-regs" if a.cache_regs else ""
    nd = os.path.join(OUT, "nd"); lift_py = os.path.join(ROOT, "lift.py"); sigf = os.path.join(nd, "src.sha1"); s = sha(exe, lift_py, extra=crs)
    if not (os.path.exists(sigf) and open(sigf).read().strip() == s and os.path.exists(os.path.join(nd, "nd_meta.json"))):
        shutil.rmtree(nd, ignore_errors=True); run([PY, "lift.py", "--smc"] + cr + [exe, "nd", nd], "lift SS2.exe"); open(sigf, "w").write(s)
    else: print("  SS2.exe: already lifted")
    run([PY, os.path.join("host", "gen_hostdata.py"), os.path.join(nd, "nd_meta.json"), exe, os.path.join(nd, "nd_hostdata.c")], "host data")
    symc = os.path.join(nd, "nd_symtab.c")                       # compiled in when present (the host treats the table as optional)
    if a.symbols:
        auto = os.path.join(nd, "auto.sym")
        if not os.path.exists(auto) or os.path.getmtime(auto) < max(os.path.getmtime(os.path.join(ROOT, "tools", "annotate.py")), os.path.getmtime(os.path.join(nd, "nd_meta.json"))):
            print("  harvesting function names (about 20 seconds)"); run([PY, os.path.join("tools", "annotate.py"), exe, nd, auto], "annotate")
        run([PY, os.path.join("host", "gen_symtab.py"), "--functions", os.path.join(nd, "nd_meta.json"), exe, symc, auto, os.path.join(ROOT, "symbols", "manual.sym")], "symbol table")
    elif os.path.exists(symc): os.remove(symc)
    mods = []
    for fname, sub, pfx, rebase in (MODULES_LOW if a.low_dll_bases else MODULES):
        src = find_ci(os.path.dirname(exe), sub, fname); md = os.path.join(OUT, pfx)
        if not os.path.isfile(src): print(f"  warning: {src} not found; the game will not run without it"); shutil.rmtree(md, ignore_errors=True); continue
        s = sha(src, lift_py, extra=str(rebase) + crs); sigf = os.path.join(md, "src.sha1")
        if not (os.path.exists(sigf) and open(sigf).read().strip() == s and os.path.exists(os.path.join(md, f"{pfx}_meta.json"))):
            print(f"  lifting {fname}", flush=True); shutil.rmtree(md, ignore_errors=True)
            run([PY, "lift.py", "--iat-indirect"] + cr + (["--rebase", hex(rebase)] if rebase else []) + [src, pfx, md], f"lift {fname}"); open(sigf, "w").write(s)
        else: print(f"  {fname}: already lifted")
        run([PY, os.path.join("host", "gen_moddata.py"), src, pfx, os.path.join(md, f"{pfx}_moddata.c")] + (["--rebase", hex(rebase)] if rebase else []), f"{fname} data")
        mods.append(pfx)
    md = os.path.join(OUT, "mods"); os.makedirs(md, exist_ok=True)
    with open(os.path.join(md, "mods.c"), "w") as f:
        f.write('#include "recomp_mod.h"\n' + "".join(f"extern const RecompModDesc {p}_desc;\n" for p in mods) + "const RecompModDesc *const recomp_mods[] = { " + "".join(f"&{p}_desc, " for p in mods) + "0 };\n")

    print("[2/2] building the host", flush=True)
    gen = "Ninja" if shutil.which("ninja") else "Unix Makefiles"
    cfg = ["cmake", "-S", os.path.join(ROOT, "port"), "-B", a.build, "-G", gen, f"-DPORT_GENERATED={OUT}", f"-DPORT_BACKEND={a.backend}", "-DCMAKE_BUILD_TYPE=Release"]
    if win:
        sys.path.insert(0, os.path.join(ROOT, "port", "tools")); import deps
        os.environ["ZIG_PY"] = sys.executable                       # used by port/cmake/zig-* (the compiler wrappers)
        cfg += [f"-DCMAKE_TOOLCHAIN_FILE={os.path.join(ROOT, 'port', 'cmake', 'windows-zig.cmake')}"]
        if a.backend == "sdl2": cfg += [f"-DSDL2_DIR={deps.sdl2_mingw(os.path.join(ROOT, 'build', 'deps'))}"]
    run(cfg, "cmake configure")
    run(["cmake", "--build", a.build, "-j", str(a.jobs)], "build")
    prog = os.path.join(a.build, "ss2port.exe" if win else "ss2port")
    if a.install:
        dest = os.path.dirname(exe); shutil.copy2(prog, dest)
        if win and os.path.isfile(os.path.join(a.build, "SDL2.dll")): shutil.copy2(os.path.join(a.build, "SDL2.dll"), dest)
        print(f"\ninstalled {os.path.basename(prog)} next to SS2.exe in {dest}")
    if win: print(f"\ndone: {prog} (with SDL2.dll). On Windows, in the game folder:  .\\ss2port.exe --windowed SS2.exe")
    else: print(f"\ndone: {prog}\nrun it from the game folder:  cd \"{os.path.dirname(exe)}\" && {prog} --windowed --verbose SS2.exe")

if __name__ == "__main__": main()
