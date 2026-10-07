#!/usr/bin/env python3
"""run_conformance.py: builds the guest conformance test (tests/guest/conformance.c) as a 32-bit Windows executable,
recompiles it with lift.py, builds the portable host around it and runs it. No game files are needed.

    python3 port/tests/run_conformance.py [--backend null|sdl2] [--build DIR] [--skip SECTION ...] [--cc CC] [--cflags ...]

Needs: python3 -m pip install pefile capstone ziglang, CMake, a host C compiler (and SDL2 for --backend sdl2).
Exit status: 0 when every check passes."""
import argparse, os, re, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__)); PORT = os.path.dirname(HERE); ROOT = os.path.dirname(PORT)
GUEST = os.path.join(HERE, "guest")
PY = sys.executable

def run(cmd, **kw):
    r = subprocess.run(cmd, **kw)
    if r.returncode: sys.exit(f"failed ({r.returncode}): {' '.join(cmd)}")

def zig_libc():
    import ziglang; return os.path.join(os.path.dirname(ziglang.__file__), "lib", "libc")

def build_guest(bdir):
    """conformance.exe: freestanding, x87 only (the recompiler does not translate SSE), at the classic 0x400000 base."""
    gd = os.path.join(bdir, "guest"); os.makedirs(gd, exist_ok=True)
    # import library for MSVCR90.dll from the prototypes in crt90.h
    names = re.findall(r"\bIMP\b[^;(]*?\b(\w+)\s*\(", open(os.path.join(GUEST, "crt90.h")).read())
    with open(os.path.join(gd, "msvcr90.def"), "w") as f: f.write("LIBRARY MSVCR90.dll\nEXPORTS\n" + "".join(n + "\n" for n in names))
    zig = [PY, "-m", "ziglang"]
    run(zig + ["dlltool", "-d", os.path.join(gd, "msvcr90.def"), "-l", os.path.join(gd, "libmsvcr90.a"), "-m", "i386"])
    lc = zig_libc(); exe = os.path.join(gd, "conformance.exe")
    run(zig + ["cc", "-target", "x86-windows-gnu", "-O1", "-march=pentium", "-mno-sse", "-mno-mmx", "-fno-stack-protector", "-fno-builtin", "-Wno-dll-attribute-on-redeclaration", "-Wno-incompatible-library-redeclaration",
               "-nostdlib", "-isystem", os.path.join(lc, "include", "any-windows-any"), "-isystem", os.path.join(lc, "include", "generic-mingw"),
               "-Wl,--entry=start@0", "-Wl,--image-base=0x400000", os.path.join(GUEST, "conformance.c"), "-L", gd,
               "-lmsvcr90", "-lkernel32", "-luser32", "-lwinmm", "-o", exe])
    return exe

def lift(exe, bdir):
    gen = os.path.join(bdir, "gen"); nd = os.path.join(gen, "nd")
    if os.path.isdir(gen): shutil.rmtree(gen)
    run([PY, os.path.join(ROOT, "lift.py"), exe, "nd", nd], stdout=subprocess.DEVNULL)
    run([PY, os.path.join(ROOT, "host", "gen_hostdata.py"), os.path.join(nd, "nd_meta.json"), exe, os.path.join(nd, "nd_hostdata.c")], stdout=subprocess.DEVNULL)
    os.makedirs(os.path.join(gen, "mods"))
    with open(os.path.join(gen, "mods", "mods.c"), "w") as f: f.write('#include "recomp_mod.h"\nconst RecompModDesc *const recomp_mods[] = { 0 };\n')
    return gen

def build_host(gen, bdir, a):
    hb = os.path.join(bdir, "host-" + a.backend)
    cfg = ["cmake", "-S", PORT, "-B", hb, "-G", "Ninja" if shutil.which("ninja") else "Unix Makefiles", f"-DPORT_GENERATED={gen}",
           f"-DPORT_BACKEND={a.backend}", "-DCMAKE_BUILD_TYPE=" + a.config]
    if a.cc: cfg.append(f"-DCMAKE_C_COMPILER={a.cc}")
    if a.cflags: cfg.append(f"-DCMAKE_C_FLAGS={a.cflags}")
    run(cfg, stdout=subprocess.DEVNULL); run(["cmake", "--build", hb], stdout=subprocess.DEVNULL)
    return os.path.join(hb, "ss2port")

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--backend", default="null"); ap.add_argument("--build", default=os.path.join(ROOT, "build", "conformance"))
    ap.add_argument("--skip", action="append", default=[]); ap.add_argument("--cc"); ap.add_argument("--cflags")
    ap.add_argument("--config", default="RelWithDebInfo"); ap.add_argument("--no-rebuild-guest", action="store_true")
    ap.add_argument("--wrap", help="run the host under this command (e.g. 'valgrind -q', 'xvfb-run -a')")
    a = ap.parse_args()
    os.makedirs(a.build, exist_ok=True)
    exe = os.path.join(a.build, "guest", "conformance.exe")
    if not (a.no_rebuild_guest and os.path.exists(exe)): exe = build_guest(a.build)
    gen = lift(exe, a.build) if not (a.no_rebuild_guest and os.path.isdir(os.path.join(a.build, "gen"))) else os.path.join(a.build, "gen")
    host = build_host(gen, a.build, a)
    work = os.path.join(a.build, "run"); shutil.rmtree(work, ignore_errors=True); os.makedirs(work)
    shutil.copy(exe, work)
    args = (a.wrap.split() if a.wrap else []) + [host, os.path.join(work, "conformance.exe")] + [x for s in a.skip for x in ("-skip", s)]
    r = subprocess.run(args, cwd=work)
    print(f"conformance ({a.backend}): {'PASS' if r.returncode == 0 else 'FAIL (exit %d)' % r.returncode}")
    sys.exit(0 if r.returncode == 0 else 1)

if __name__ == "__main__": main()
