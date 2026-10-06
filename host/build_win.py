#!/usr/bin/env python3
"""Build ss2_native.exe from your own SS2.exe (NewDark 2.48). Works on Windows (PowerShell), Linux and macOS.

    python host/build_win.py "C:\\Games\\System Shock 2\\SS2.exe"
    python host/build_win.py "C:\\Games\\System Shock 2\\SS2.exe" --install     # also copy the result next to SS2.exe

Steps: (1) lift SS2.exe (and the script modules Data\\allobjs.osm and osm\\Squirrel.osm, if present) to C (about 8 min, skipped if already done), (2) compile everything with Zig (about 10 min,
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
    ap.add_argument("--no-osm", action="store_true", help="don't recompile the script modules (the game then uses the original DLLs)")
    ap.add_argument("--hooks", help="hooks file for tools/apply_hooks.py: call your own C functions at the entry of recompiled functions (for mods)")
    ap.add_argument("--extra-src", nargs="*", default=[], help="extra C files to compile and link into the exe (for mods; they see runtime/rt.h)")
    ap.add_argument("--named-sources", action="store_true", help="also write out/nd_named: a copy of the generated C with function names and notes, for reading")
    a = ap.parse_args()
    exe = os.path.abspath(a.ss2exe)
    if not os.path.isfile(exe): sys.exit(f"SS2.exe not found: {exe}")
    for mod in ("pefile", "capstone", "ziglang"):
        try: __import__(mod)
        except ImportError: sys.exit(f"Missing Python package '{mod}'. Run:  python -m pip install pefile capstone ziglang")
    gen = os.path.join(ROOT, "out", "nd"); objd = os.path.join(ROOT, "build", "win"); os.makedirs(objd, exist_ok=True)
    t0 = time.time()

    step(1, "Lifting SS2.exe to C (about 8 minutes the first time)")
    import hashlib
    esig = hashlib.sha1(open(exe, "rb").read() + open(os.path.join(ROOT, "lift.py"), "rb").read()).hexdigest(); esigf = os.path.join(gen, "src.sha1")
    if os.path.exists(os.path.join(gen, "nd_meta.json")) and os.path.exists(esigf) and open(esigf).read().strip() == esig: print("  already done, skipping")
    else:
        if os.path.isdir(gen):
            print("  the lifter or SS2.exe changed since the last build: lifting again")
            shutil.rmtree(gen); [os.remove(o) for o in glob.glob(os.path.join(objd, "nd_*.o"))]
        run([PY, "lift.py", "--smc", exe, "nd", gen], "lift")
        open(esigf, "w").write(esig)
    run([PY, os.path.join("host", "gen_hostdata.py"), os.path.join(gen, "nd_meta.json"), exe, os.path.join(gen, "nd_hostdata.c")], "host data")

    # names: harvested from your exe (RTTI, vtables, strings) + symbols/manual.sym; used in crash reports
    auto = os.path.join(gen, "auto.sym")
    if not os.path.exists(auto) or os.path.getmtime(auto) < os.path.getmtime(os.path.join(ROOT, "tools", "annotate.py")):
        print("  harvesting function names (about 20 seconds)")
        run([PY, os.path.join("tools", "annotate.py"), exe, gen, auto], "annotate")
    syms = [auto, os.path.join(ROOT, "symbols", "manual.sym")]
    run([PY, os.path.join("host", "gen_symtab.py"), exe, os.path.join(gen, "nd_symtab.c")] + syms, "symbol table")
    if a.named_sources:
        run([PY, os.path.join("tools", "name_sources.py"), gen, os.path.join(ROOT, "out", "nd_named"), *syms], "named sources")
        print("  readable copy with names: out/nd_named (functions.txt is the index)")

    # script modules: recompiled too when present; the host swaps them in at runtime (runtime/recomp_mod.h)
    #   (file name, folder next to SS2.exe, prefix, base to lift at: None = its own preferred base)
    MODULES = [("allobjs.osm", "Data", "ao", None),
               ("Squirrel.osm", "osm", "sq", 0x30000000)]   # same preferred base as allobjs.osm: mapped elsewhere by the host
    mods = []
    for fname, sub, pfx, rebase in ([] if a.no_osm else MODULES):
        src = os.path.join(os.path.dirname(exe), sub, fname); md = os.path.join(ROOT, "out", pfx)
        if not os.path.isfile(src):
            print(f"  (no {src}: it will run as the original DLL)")
            continue
        import hashlib; sig = hashlib.sha1(open(src, "rb").read() + str(rebase).encode() + open(os.path.join(ROOT, "lift.py"), "rb").read()).hexdigest()
        sigf = os.path.join(md, "src.sha1")
        if os.path.exists(os.path.join(md, f"{pfx}_meta.json")) and os.path.exists(sigf) and open(sigf).read().strip() == sig:
            print(f"  {fname}: already lifted")
        else:
            print(f"  lifting {fname} ({'about 15 seconds' if pfx == 'ao' else 'about 4 minutes'})")
            shutil.rmtree(md, ignore_errors=True)
            for o in glob.glob(os.path.join(objd, f"{pfx}_*.o")): os.remove(o)
            run([PY, "lift.py", "--iat-indirect"] + (["--rebase", hex(rebase)] if rebase else []) + [src, pfx, md], f"lift {fname}")
            open(sigf, "w").write(sig)
        run([PY, os.path.join("host", "gen_moddata.py"), src, pfx, os.path.join(md, f"{pfx}_moddata.c")] + (["--rebase", hex(rebase)] if rebase else []), f"{fname} data")
        mods.append((pfx, md))
    for _, _, pfx, _ in MODULES:                 # drop objects of modules not built this time
        if pfx not in [m[0] for m in mods]:
            for o in glob.glob(os.path.join(objd, f"{pfx}_*.o")): os.remove(o)
    modsc = os.path.join(ROOT, "out", "mods", "mods.c"); os.makedirs(os.path.dirname(modsc), exist_ok=True)
    txt = '#include "recomp_mod.h"\n' + "".join(f"extern const RecompModDesc {p}_desc;\n" for p, _ in mods) + \
          "const RecompModDesc *const recomp_mods[] = { " + "".join(f"&{p}_desc, " for p, _ in mods) + "0 };\n"
    if not os.path.exists(modsc) or open(modsc).read() != txt: open(modsc, "w").write(txt)

    # mod support: hooks into the generated C (seconds, no re-lift) and extra sources
    r = run([PY, os.path.join("tools", "apply_hooks.py")] + (["--hooks", os.path.abspath(a.hooks)] if a.hooks else []) + [gen] + [md for _, md in mods], "hooks")
    if a.hooks: print("  " + r.stdout.strip())
    extra = [os.path.abspath(x) for x in a.extra_src]
    for x in extra:
        if not os.path.isfile(x): sys.exit(f"extra source not found: {x}")
    keep = {"extra_" + os.path.basename(x)[:-2] + ".o" for x in extra}
    for o in glob.glob(os.path.join(objd, "extra_*.o")):
        if os.path.basename(o) not in keep: os.remove(o)

    step(2, "Compiling (about 10 minutes the first time; later runs only rebuild what changed)")
    cf = ["-target", "x86-windows-gnu", "-O1", "-fno-sanitize=undefined", "-fno-stack-protector", "-fno-strict-aliasing", "-fwrapv", "-w",
          "-DRT_IDENTITY", "-DRT_RING", "-DHOST_BUILD=\"dev\"", "-Iruntime"]
    inc = {gen: ["-I" + os.path.relpath(gen, ROOT)], os.path.dirname(modsc): []}
    for _, md in mods: inc[md] = ["-I" + os.path.relpath(md, ROOT)]
    hostflags = ["-I" + os.path.relpath(gen, ROOT)]
    zig = [PY, "-m", "ziglang", "cc"]
    jobs = []
    run(zig + ["-target", "x86-windows-gnu", "-c", os.path.join("host", "guest_region.s"), "-o", os.path.join("build", "win", "00_guest_region.o")], "guest region")
    srcs = sorted(glob.glob(os.path.join(gen, "*.c"))) + [c for _, md in mods for c in sorted(glob.glob(os.path.join(md, "*.c")))] + [modsc, os.path.join(ROOT, "host", "win_host.c")] + extra
    deps = os.path.getmtime(os.path.join(ROOT, "runtime", "rt.h"))
    stamp = os.path.join(objd, "host_flags.txt"); hf = " ".join(hostflags)
    host_changed = not os.path.exists(stamp) or open(stamp).read() != hf
    for s in srcs:
        o = os.path.join(objd, ("extra_" if s in extra else "") + os.path.basename(s)[:-2] + ".o")
        dep = deps
        if s.endswith(("_moddata.c", "win_host.c")) or s == modsc: dep = max(deps, os.path.getmtime(os.path.join(ROOT, "runtime", "recomp_mod.h")))
        need = not os.path.exists(o) or os.path.getmtime(o) < max(os.path.getmtime(s), dep) or (s.endswith("win_host.c") and host_changed)
        if need: jobs.append((s, o))
    done = [0]
    def comp(j):
        s, o = j
        fl = hostflags if s.endswith("win_host.c") or s in extra else inc[os.path.dirname(s)]
        run(zig + cf + fl + ["-c", s if s in extra else os.path.relpath(s, ROOT), "-o", os.path.relpath(o, ROOT)], "compile " + os.path.basename(s))
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
