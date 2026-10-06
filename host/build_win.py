#!/usr/bin/env python3
"""Build ss2_native.exe from your own SS2.exe (NewDark 2.48). Works on Windows (PowerShell), Linux and macOS.

    python host/build_win.py "C:\\Games\\System Shock 2\\SS2.exe"
    python host/build_win.py "C:\\Games\\System Shock 2\\SS2.exe" --install     # also copy the result next to SS2.exe

Steps: (1) lift SS2.exe (and Data\\allobjs.osm, if present) to C (about 8 min, skipped if already done), (2) compile everything with Zig (about 10 min,
only changed files are rebuilt next time), (3) link and fix up the exe.
Needs:  python -m pip install pefile capstone ziglang
"""
import argparse, glob, os, shutil, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = sys.executable
env = dict(os.environ)
cache = os.path.join(ROOT, "build", "zigcache")
env.setdefault("ZIG_GLOBAL_CACHE_DIR", cache); env.setdefault("ZIG_LOCAL_CACHE_DIR", cache)

def run(cmd, what):
    r = subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True, text=True)
    if r.returncode:
        print(f"\nFAILED: {what}\n  {' '.join(cmd)}\n{r.stdout}{r.stderr}"); sys.exit(1)
    return r

def step(n, msg): print(f"\n[{n}/3] {msg}", flush=True)

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ss2exe", help="path to your NewDark 2.48 SS2.exe")
    ap.add_argument("--out", default=os.path.join("build", "win", "ss2_native.exe"), help="where to put the result")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2, help="parallel compile jobs")
    ap.add_argument("--install", action="store_true", help="copy the built exe into the folder that contains SS2.exe")
    ap.add_argument("--osm", help="allobjs.osm to recompile as well (default: Data\\allobjs.osm next to SS2.exe, if present)")
    ap.add_argument("--no-osm", action="store_true", help="don't recompile allobjs.osm (the game then uses the original DLL)")
    a = ap.parse_args()
    exe = os.path.abspath(a.ss2exe)
    if not os.path.isfile(exe): sys.exit(f"SS2.exe not found: {exe}")
    for mod in ("pefile", "capstone", "ziglang"):
        try: __import__(mod)
        except ImportError: sys.exit(f"Missing Python package '{mod}'. Run:  python -m pip install pefile capstone ziglang")
    gen = os.path.join(ROOT, "out", "nd"); objd = os.path.join(ROOT, "build", "win"); os.makedirs(objd, exist_ok=True)
    t0 = time.time()

    step(1, "Lifting SS2.exe to C (about 8 minutes the first time)")
    if os.path.exists(os.path.join(gen, "nd_meta.json")): print("  already done, skipping")
    else: run([PY, "lift.py", "--smc", exe, "nd", gen], "lift")
    run([PY, os.path.join("host", "gen_hostdata.py"), os.path.join(gen, "nd_meta.json"), exe, os.path.join(gen, "nd_hostdata.c")], "host data")

    # allobjs.osm (the game's object scripts): recompiled too when available; the host swaps it in at runtime
    osm = None if a.no_osm else os.path.abspath(a.osm) if a.osm else os.path.join(os.path.dirname(exe), "Data", "allobjs.osm")
    if osm and not os.path.isfile(osm):
        if a.osm: sys.exit(f"allobjs.osm not found: {osm}")
        print(f"  (no {osm}: allobjs.osm will run as the original DLL)"); osm = None
    aod = os.path.join(ROOT, "out", "ao")
    if osm:
        import hashlib; sig = hashlib.sha1(open(osm, "rb").read()).hexdigest(); sigf = os.path.join(aod, "osm.sha1")
        if os.path.exists(os.path.join(aod, "ao_meta.json")) and os.path.exists(sigf) and open(sigf).read().strip() == sig:
            print("  allobjs.osm: already lifted")
        else:
            print("  lifting allobjs.osm (about 15 seconds)")
            shutil.rmtree(aod, ignore_errors=True)
            for o in glob.glob(os.path.join(objd, "ao_*.o")): os.remove(o)
            run([PY, "lift.py", "--iat-indirect", osm, "ao", aod], "lift allobjs.osm")
            open(sigf, "w").write(sig)
        run([PY, os.path.join("host", "gen_moddata.py"), osm, "ao", os.path.join(aod, "ao_moddata.c")], "allobjs.osm data")
    else:
        for o in glob.glob(os.path.join(objd, "ao_*.o")): os.remove(o)

    step(2, "Compiling (about 10 minutes the first time; later runs only rebuild what changed)")
    cf = ["-target", "x86-windows-gnu", "-O1", "-fno-sanitize=undefined", "-fno-stack-protector", "-fno-strict-aliasing", "-fwrapv", "-w",
          "-DRT_IDENTITY", "-DRT_RING", "-DHOST_BUILD=\"dev\"", "-Iruntime"]
    inc = {gen: ["-I" + os.path.relpath(gen, ROOT)], aod: ["-I" + os.path.relpath(aod, ROOT)]}
    hostflags = ["-I" + os.path.relpath(gen, ROOT)] + (["-DHAVE_AO"] if osm else [])
    zig = [PY, "-m", "ziglang", "cc"]
    jobs = []
    run(zig + ["-target", "x86-windows-gnu", "-c", os.path.join("host", "guest_region.s"), "-o", os.path.join("build", "win", "00_guest_region.o")], "guest region")
    srcs = sorted(glob.glob(os.path.join(gen, "*.c"))) + (sorted(glob.glob(os.path.join(aod, "*.c"))) if osm else []) + [os.path.join(ROOT, "host", "win_host.c")]
    deps = os.path.getmtime(os.path.join(ROOT, "runtime", "rt.h"))
    stamp = os.path.join(objd, "host_flags.txt"); hf = " ".join(hostflags)
    host_changed = not os.path.exists(stamp) or open(stamp).read() != hf
    for s in srcs:
        o = os.path.join(objd, os.path.basename(s)[:-2] + ".o")
        need = not os.path.exists(o) or os.path.getmtime(o) < max(os.path.getmtime(s), deps) or (s.endswith("win_host.c") and host_changed)
        if need: jobs.append((s, o))
    done = [0]
    def comp(j):
        s, o = j
        extra = hostflags if s.endswith("win_host.c") else inc[os.path.dirname(s)]
        run(zig + cf + extra + ["-c", os.path.relpath(s, ROOT), "-o", os.path.relpath(o, ROOT)], "compile " + os.path.basename(s))
        done[0] += 1; print(f"  compiled {done[0]}/{len(jobs)}: {os.path.basename(s)}", flush=True)
    print(f"  {len(jobs)} of {len(srcs)} files to compile with {a.jobs} jobs")
    with ThreadPoolExecutor(a.jobs) as ex: list(ex.map(comp, jobs))
    open(stamp, "w").write(hf)

    step(3, "Linking")
    out = os.path.abspath(os.path.join(ROOT, a.out)); os.makedirs(os.path.dirname(out), exist_ok=True)
    objs = sorted(glob.glob(os.path.join(objd, "*.o")))
    run(zig + ["-target", "x86-windows-gnu", "-mwindows", "-o", out] + objs + ["-Wl,--image-base=0x400000", "-Wl,--stack,1048576", "-luser32", "-lkernel32", "-lm"], "link")
    run([PY, os.path.join("host", "fix_pe.py"), out], "fix_pe")
    print(f"\nBuilt {out}  ({(time.time() - t0) / 60:.1f} min)")
    if a.install:
        dst = os.path.join(os.path.dirname(exe), "ss2_native.exe"); shutil.copy2(out, dst); print(f"Copied to {dst}")
    else:
        print(f'Next: copy it into your System Shock 2 folder (next to SS2.exe) and run it, or re-run with --install.')

if __name__ == "__main__": main()
