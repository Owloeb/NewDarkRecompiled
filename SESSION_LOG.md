# darkrecomp: session log & handoff (2026-10-01 → 2026-10-02)

Context for a Claude Code session picking this project up. Owen is the owner. Pair this with the `darkrecomp/`
source tree (Darkrecomp.tar / NewDarkrecomp.tar), README.md and NEWDARK.md.

## Goal (as it evolved)

1. Started: static recompilation of the original System Shock 2 for a homebrew **PS Vita**. Owen previously hit a
   performance wall porting Ion Fury to the Vita.
2. Now: **VR mod built on NewDark 2.48** is the primary track. The Vita is on the back burner but shares the
   runtime/D3D-layer work.

## Ground rules decided

- **No leaked source.** Owen offered GitHub repos of the leaked Dark Engine source (DeathEngine2/LookingGlass-DarkEngine,
  dima424658/darkengine). We declined: everything is derived from retail binaries Owen owns, keeping the project
  clean and shareable. Don't pull those repos in.
- **Don't distribute generated C** (it's derived from Nightdive's copyrighted code). Ship the tools; users regenerate
  from their own exe, the N64Recomp model. Mods would ship as patches.
- The 25th Anniversary Remaster (Nightdive KEX engine, 64-bit, already on consoles) is out of scope.

## Inputs (uploaded, in the cloud workspace; Owen has the originals)

| File | Notes |
|---|---|
| SS2.exe "legacy" 2.3 (Sep 1999) | from GOG forum "legacy OldDark" patch; PE32, base 0x400000, **no relocs**, 2.26 MB code |
| NewDark SS2.exe 2.48 (Jun 2019) | fresh GOG install; MSVC 2008, **has relocs**, 3.2 MB code, imports MSVCR90/MSVCP90 |
| allobjs.osm | SS2 gameplay script module; **byte-identical** in both installs (sha256 096c3a1e…a645) |
| squirrel.osm | NewDark-era Squirrel 3.1 scripting bridge (2018); needs NewDark's script API, unused by OldDark |

## What was built (`darkrecomp/`)

- `lift.py`: x86 PE → portable C. Function discovery (relocs, or data-section pointer scan for reloc-less images, plus
  immediates), CFG, per-instruction C emission (integer, x87 as `double`, string ops, `fs:`), jump tables (incl.
  negative-index and gapped MSVC `memcpy` tables), import/indirect dispatch.
  - `--smc`: maps every absolute write into code to the exact instruction field it patches. Patched instructions read
    that field from guest memory at runtime; a patched branch becomes a `switch` over every value written to it.
  - **Interprocedural flags analysis**: CRT asm returns results in EFLAGS across `ret`/`call`, so spills and reloads
    are emitted only where needed. Indirect calls are treated as flag boundaries.
  - Memory-lean (bounded decode cache): NewDark lifts in ~1 min / ~2 GB RAM.
- `runtime/rt.h`: CPU struct (regs, x87 stack, flags crossing calls, fs_base), guest memory `M + addr`, x87 helpers,
  `CALLPUSH`/`RETCHK` (shadow stack in `RT_SHADOW`), `BUDGET` (`RT_BUDGET`), `TRACE` (`RT_TRACE`).
- `harness/`:
  - `hx.c`: runs one recompiled function on a given state.
  - `difftest.py`: function-level differential test vs Unicorn running the original x86; compares registers, x87 and
    all writable memory, also at the fault point. For self-modifying binaries code is writable, and a write hook flags
    code writes outside the known patch fields.
  - `insnfuzz.py`: every distinct instruction encoding in both binaries wrapped in a stub and fuzzed, comparing the
    Intel-defined flags.
  - `smc_test.py`: targeted test for the self-patching renderer (`SMC_SMALL=1 SMC_PREPATCH=1` pre-loads live patch
    values and randomizes renderer globals).
  - `tracecmp.py`: lockstep per-instruction trace, reporting the first divergence (needs a `build_trace.sh` build).
  - `cats.py`: summarizes a report.
- `build.sh <gen> <objdir> <lib> <prefix>`, `build_trace.sh`.

## Verification results

- **SS2.exe 2.3**: 16,395 functions → 111 C files.
  - Function level: 7,421 match on completion and 8,845 match at the fault point; 51 junk-only, 39 identical faults
    only, 7 budget.
  - 8 remaining failures, all explained: 80-bit vs double exponent range (3), NaN sign (1), GCC dropping dead
    faulting loads (3, identical in the trace build), junk decode (1).
  - 10 fault-point-only differences, all traced, with no divergence before the fault.
- **Self-modifying renderer**: 237 patch fields + 1 patched `jg` (span routine at 0x5af829, first-hit path).
  459 passing trials executed live-patched instructions across 18 of 20 functions, with 0 real mismatches.
- **allobjs.osm**: 3,716 functions; 1,995 pass and 1,711 pass at the fault point. 2 known failures: NaN payload and
  `fxam` on an empty x87 register.
- **Instruction fuzzer**: 308,039 of 308,049 trials pass. Failing shapes: a Unicorn `fyl2x` bug and subnormal `fmul`.
- **Negative control**: a planted `sar`→`shr` bug was caught in 13/13 shapes by the fuzzer (only 2/38 at function
  level, so both layers matter).

## Bugs found and fixed along the way

- Flags crossing call/return (CRT `_ftol`/float helpers return NaN/Inf in ZF).
- MSVC `memcpy` jump tables: negative indices and an unused first slot.
- x87 `fsin`/`fcos`/`fptan`/`fsincos` with |x| ≥ 2^63 must leave the operand and set C2.
- Pointer-scanning code sections of reloc-less images created fake entries at every patch target.
  The 0x52e100–0x52e650 mapper variant turned out unreferenced and is treated as dead (dispatch fails loudly).
- Harness issues: stale Unicorn state across faulting runs (fresh `Uc` per trial), Unicorn needs a GDT with a flat
  32-bit SS plus an FS descriptor, and random pointers must not target code.

## Known gaps

- x87 tag word (empty regs) and the AF flag aren't modelled.
- x87 precision control is ignored (always 53-bit; D3D sets 24-bit).
- NaN payloads, subnormals and the 80-bit range differ from real x87.
- Not atomic at a fault.
- Return addresses don't steer control flow, so setjmp/longjmp and C++ EH need runtime support.
- No optimization pass yet.

## NewDark 2.48 findings (lifted, NOT yet compiled or verified)

- 23,157 functions → 156 C files; 239 SMC fields + the same patched branch, 0 unresolved.
- Flags analysis: 102 functions read flags on entry, 19 export them.
- Floating point is x87 (VS2008 default). SSE2 is confined to ~10 functions:
  - 0x519930 family: 16-bit pixel blend/scale
  - 0x6c6700: memset
  - 0x6f77fe: CPUID
  - a few one-off CRT instructions
  - Options: add ~15 SSE2 instructions to the lifter and fuzz them, or have CPUID report no SSE2.
- New forms: `fcomip`, `repz ret`.
- C++ EH: `__CxxFrameHandler3` frames in 1,346 functions (mostly destructor cleanup), only **one**
  `_CxxThrowException` site. v1 treats a throw as fatal.
- Runtime DLLs: d3d9 + d3dx9_43, ddraw, dsound/openal32, dinput, lgvid.dll (video), fmsel.dll (FM selector),
  darkdlgs.dll, MSVCR90/MSVCP90.

## Plan (VR on NewDark)

1. **Compile and verify NewDark**: difftest, insnfuzz regenerated for its encodings, smc_test. Use the CPUID trick
   initially.
2. **Windows host, identity-mapped** (guest address == host address, 32-bit Windows build):
   - imports pass straight through to the real DLLs, including real D3D9
   - trampolines for callbacks into guest code: WndProc, timers, enumerators, thread starts
   - minimal SEH via `fs:[0]`
   - load allobjs and squirrel.osm
   - boot NewDark on Owen's PC
   - Build on Owen's Windows machine, or cross-compile with clang targeting i686-windows and send the exe.
3. **Find functions** via call logging, function swapping and frame replay: camera setup, world render, weapon/hand
   drawing, aiming, HUD.
4. **VR**:
   - render the world twice per frame into D3D9 offscreen targets *without stepping the simulation twice*
   - submit to OpenXR
   - head tracking decoupled from body
   - tracked hands/weapon with a **left-handed option** (Owen modded BioShock VR for left-handed play and two-handed
     grips)
   - HUD/inventory on a floating panel with a laser pointer
5. Later: Vita backend (vitaGL) for the same D3D layer, using the OldDark exe (better CPU fit); Win32 shim needed there.

## Open questions for Owen

- Which headset / runtime (SteamVR, Quest Link, WMR)? Assumed PCVR via OpenXR.
- Build on his Windows machine, or have Claude cross-compile?

## Handy commands

```sh
python3 lift.py --smc SS2.exe ss out/ss2 && ./build.sh out/ss2 build/ss build/libss.so ss
python3 lift.py --smc NewDark/SS2.exe nd out/nd && ./build.sh out/nd build/nd build/libnd.so nd
UC_IGNORE_REG_BREAK=1 python3 harness/difftest.py SS2.exe out/ss2/ss_meta.json build/libss.so --trials 4 --report ss.json
python3 harness/cats.py ss.json
SMC_SMALL=1 SMC_PREPATCH=1 python3 harness/smc_test.py SS2.exe out/ss2/ss_meta.json build/libss.so out/ss2
python3 harness/insnfuzz.py gen out/fz allobjs.osm SS2.exe && ./build.sh out/fz build/fz build/libfz.so fz
python3 harness/insnfuzz.py run out/fz build/libfz.so --trials 24
./build_trace.sh out/ss2 build/sst build/libsst.so ss
python3 harness/tracecmp.py SS2.exe out/ss2/ss_meta.json build/libsst.so 0x<func> <trial>
```

Dependencies: python3, pefile, capstone, unicorn, gcc/clang. The difftest needs a 64-bit Linux host (reserves 4 GB
for guest memory). The full exe difftest takes ~35 min on 2 cores; run it in the background.

## Update 2026-10-02: step 1 done (NewDark compiled and verified)

See NEWDARK.md "Verification". Lifter changes: fcomi family, cpuid (no SSE2), repz ret, and a discovery fix for relocated
images (phantom patch-field entries). Harness: SMC_GEN honoured by tracecmp; fuzzer checks fcomi OF/SF against the SDM,
masks AF for lahf, adds NaN/inf inputs for compare shapes. `triage.sh <verdict> <outfile>` lockstep-traces every function
with a given verdict. Next: step 2 (Windows identity-mapped host, boot with real D3D9).
