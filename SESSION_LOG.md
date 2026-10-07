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

## Update 2026-10-06 → 2026-10-07: portable host (branch `portable-host`, not merged)

Scope: roadmap item 3, a platform layer so the recompiled game runs beyond 32-bit Windows. The repo stays a **purely
vanilla recompilation**; VR/mods live in a separate repo and are paused. Goals: a complete recompile and maximum
portability (Android, ARM, consoles; eventually a PS Vita homebrew port, later Thief Gold/Thief 2). Back ends for
specific platforms are the porter's job; this repo ships the shared layer (`port/`), a Linux build and a measured
D3D9 usage list.

### Already on main before this work
- Built-in cutscene decoder (`video/`), replacing `ffmpeg.dll` (15535b2). Release v0.2.0 is for Owen to publish
  (the session's token gets HTTP 403 on release creation). Stale branches `squirrel-osm`, `lgvid-fmsel`,
  `builtin-video` can be deleted by Owen.

### What `port/` is
- `host.c` entry/loader/fault reporting, `win32.c` + `win32b.c` Win32 API, `crt.c` MSVC CRT, `com.c` null D3D9 /
  DirectSound / DirectInput / DirectDraw back ends, `mmio.c`, `modules.c` (recompiled-module loader), `build.sh`,
  `README.md`.
- Non-identity guest memory: `GP(a) = M + (uint32_t)a`. Guest map: exe 0x400000.., stack top 0x08400000, modules
  ao 0x10000000 / sq 0x30000000 / lv 0x30300000 / fm 0x30400000, heap 0x40000000.., VirtualAlloc 0x80000000..,
  fake DLL handles 0xEE000000+, thunks 0xF0000000+.
- Shim convention: args from guest stack `A(i)`, return in eax (`RET`) or x87 (`RETF`); callee pop is declared per
  shim (`STD(n)` / `CDECL`). A wrong pop count shifts the guest stack and usually surfaces later as a security
  cookie failure or a nonsense value.
- COM objects are 16-byte guest structs `[vtable, refcount, class, host Obj index]`; vtables are built from
  "Method:nargs" spec strings (nargs excludes `this`).
- Options: `--list-missing --list-shims --selftest --trace --frames N`; env `PORT_FILES=1` (log path resolution),
  `PORT_TRACE=1`. Debug build (`-DRT_TRACE`): `PORT_COV=file`, `PORT_AT=addr,..`, `PORT_MEM=addr,..`, `PORT_HEX=1`,
  `PORT_WATCH=addr` (who changes a guest word). Faults print registers, last host calls, an ebp-chain backtrace
  and stack words.
- Also: opt-in D3D9/API usage recorder for the Windows host (`darkrecomp_apistats.txt`, `host/api_usage.inc`);
  compile-checked only, not yet run on Windows.

### Working loop
Owen runs the static Linux binary in WSL2 against his real game folder and sends the log; the session diagnoses,
fixes, rebuilds and ships a new zip (or the trace build as `.xz`, the zip exceeded the 30 MiB upload limit).

### Bugs found and fixed (in order)
1. Stack cookie failure: `_beginthreadex` was declared `STD(6)` but the guest cleans its own arguments. Now `CDECL`.
2. All resource folders rejected: `_getdcwd(3, NULL, n)` / `_getcwd(NULL, n)` must allocate the buffer when it is
   NULL. Fixed; resource search paths (`osm`, `patch`, `Data`, `Data\res`, `.crf`/`.zip` probes) now register.
3. File names with a trailing `\r`: `fopen("r")` emulates Windows text mode (CRLF to LF) for config files.
4. Missing files are now logged (`port_miss`) so absent game data is visible.
5. Divide error at 006c999d in the 2D overlay draw (`6c9800`): not a rect problem. The two
   `IDirect3DStateBlock9` objects (`Capture` / `Apply`, vtable slots 4 and 5) were created from the generic class
   whose slots 4/5 are `GetDeclaration:2` / `GetFunction:2`, so each call popped 8 bytes too many. Added a
   dedicated `C_SB` class (`GetDevice:1 Capture:0 Apply:0`) used by `CreateStateBlock` and `EndStateBlock`.
   Lesson: generic COM classes must not share slot layouts across different interfaces.
6. `operator new(2374864012)` after `IDirectSoundBuffer::GetFormat`: the game asks for the format size first
   (NULL buffer, size out in the 4th argument) then allocates it. Implemented `GetFormat` / `SetFormat`; a buffer
   with no format reports a default 44.1 kHz 16-bit stereo PCM record. Shipped, awaiting Owen's next run.

### State at end of session
- The game boots to `CreateDevice 640x480` and the first `Present` with the null renderer (no window, picture,
  sound or input yet), then continues into sound setup.
- Not done: fmsel.dll loading (intentionally refused), C++ exceptions/SEH, threads (`_beginthreadex` returns 0;
  sound mixer/timer threads are skipped), WinMM timer callbacks, WinSock ordinals imported by Squirrel.osm,
  real rendering/audio/input back ends, a size-optimised build for the Vita (the Linux binary is about 69 MB;
  the Vita has a 444 MHz CPU and little RAM), the measured D3D9 usage list, updating `port/README.md`.
- Annotating addresses (naming functions/globals) is separate from the platform layer.

### Rules still in force
- Do not merge `portable-host` to main until Owen confirms.
- Generated C is not distributed; ship tools only.
- Debugging aid: the sandbox has no game data, so faults can only be reproduced from Owen's logs.

### Next
Run the latest zip, fix each fault Owen reports until the game reaches the main menu with null graphics/audio,
then update `port/README.md`, produce the D3D9 usage list, and try the Windows API recorder.

## Session: platform-layer branch (restart of portable-host)

Decision: the old `portable-host` branch was audited, not trusted. Its null COM objects had real bugs (wrong pop counts,
shared generic COM classes, handle collisions, missing IDirectSound3DListener methods). `platform-layer` is a rewrite
branched from main (portable-host untouched). Goal: roadmap item 3, platform-agnostic, not tied to one console.

Done: `port/include/plat.h` interface; shared core/win32/crt/dx front ends; backends posix, null, sdl2 (GL 2.1 / ES 2.0);
real guest threads with loop-head preemption (`RT_PREEMPT` in `runtime/rt.h`); D3D9, DirectSound mixer, DirectInput
front ends; built-in cutscene decoder wired in (`dx/video.c`, `PORT_VIDEO`); static checkers and a game-free
conformance suite (`port/tests/run_conformance.py`: 370 null / 397 sdl2 checks, ASan/UBSan/TSan clean).

Not verified: the real SS2.exe (no game data in the sandbox). Next step is Owen running it on his game folder and sending
the log of the first fault. Rules still in force: do not merge to main until Owen confirms; generated C is never
distributed; stay vanilla.

### First runs on Owen's machine (WSL, Ubuntu, Windows 10)
- Builds and starts; recompiled lgvid.dll loads; WSLg window with Mesa llvmpipe GL.
- Crash at 0x4aff44 (call through NULL after a failed `iface\fontpal` lookup). Root cause found with the new
  `--trace-only` file trace: the engine's ZIP (.crf) reader sizes archives with `_filelength(_fileno(f))`; `_fileno`
  of an fopen stream is 1000+index, which `_filelength`/`_fstat` did not accept, so every archive had length -1, the
  end-of-directory seek (-23) failed, and the reader indexed garbage (also the ~224 MB heap growth and the endless loop
  seen under full tracing). Fixed in port/crt/stdio.c; regression check added to the conformance test.
- Tracing is now bounded (first 8192 + last 4096 calls in memory, with return values and callers): the first version
  streamed every call and filled the disk.
