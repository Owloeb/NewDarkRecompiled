# darkrecomp

A static recompiler that turns the 32-bit x86 binary of **System Shock 2 (NewDark 2.48)** into portable C, plus a small
Windows host that builds that C into a native executable you run in place of `SS2.exe`.

**Status: playable on flat screen.** Boot, main menu, character creation, all levels, level changes, save/load,
in-game UI (HUD, inventory), audio, input, fullscreen and cutscenes all run through recompiled code.
This is a work in progress, not a finished product (see *Known issues*).

## Why this exists

System Shock 2's engine source was never released, so the only way to change how the engine works has been patching the
original binary. This project turns the binary into C that can be rebuilt, read and changed: a **vanilla recompilation**
that plays exactly like the original, as a foundation for others to build on. Fixes, mods that need engine-level access
(a VR version is being built as a separate project on top of this one), and ports to other platforms all start from here.

This repository stays vanilla: it reproduces the original game and adds nothing to it.

## What it is, in one paragraph

`lift.py` reads your own `SS2.exe`, finds every function (about 21,000), and emits one C function per x86 function.
The Windows host (`host/win_host.c`) is linked at the game's original address (0x400000). At start-up it loads your
`SS2.exe` sections into that range, wires the import table to the real Windows API, and redirects every original function
to its recompiled version. Window procedures, DirectX callbacks, the C runtime's static constructors and script modules
that call into the engine land in recompiled code too. The script modules (`allobjs.osm`, the game's object scripts, and
`Squirrel.osm`, NewDark's Squirrel scripting) are recompiled the same way: when the engine loads one, the host loads the
file itself, checks it is the exact file that was recompiled, and runs the recompiled code instead (falling back to the
original if a mod ships its own copy). Both modules want the same address, so `Squirrel.osm` is lifted as if loaded at
0x30000000 and the host maps and relocates it there. The other Looking Glass DLLs (`lgvid.dll`, `fmsel.dll`,
`darkdlgs.dll`) still run as the original binaries for now.

## Legal and ground rules

The tools here are MIT-licensed (see LICENSE). The game, its engine and anything generated from it are not.

- No leaked Dark Engine source was used. Everything is derived from binaries by analysis and testing.
- The generated C and the built exe are derived from the copyrighted game binary. **They are not distributed.**
  This repository ships only the tools; you generate everything from a copy of the game you own.
- You need a legitimate install of System Shock 2 with the NewDark 2.48 executable (the GOG and Steam releases are the usual source).
  The 25th Anniversary Remaster is a different engine and is out of scope.

## Quick start (Windows / PowerShell)

1. Install [Python 3](https://www.python.org/downloads/) (tick "Add python.exe to PATH" in the installer).
2. Open PowerShell in the folder where you unpacked this project (the folder containing `lift.py`) and install the three
   Python packages it needs. The compiler (Zig) comes from the `ziglang` package, so there is nothing else to download:

        python -m pip install pefile capstone ziglang

3. Build, pointing at your own NewDark 2.48 `SS2.exe` (put the path in quotes) and adding `--install` to copy the result
   straight into that folder:

        python host\build_win.py "C:\Games\System Shock 2\SS2.exe" --install

   The first build takes about 20 minutes (about 8 to convert the game to C, about 10 to compile). It prints progress as it
   goes. Later builds only redo what changed. `Data\allobjs.osm` and `osm\Squirrel.osm` next to your `SS2.exe` are found
   and recompiled automatically (`--no-osm` skips them).

4. Run `ss2_native.exe` from your System Shock 2 folder instead of `SS2.exe`. Leave everything else in that folder where it
   is: the game's data, `lgvid.dll`, `ffmpeg.dll`, `fmsel.dll`, `darkdlgs.dll`, `allobjs.osm`, `Squirrel.osm` and the config
   files. Display and audio settings come from the game's own config, so whatever you use in the normal game applies here.

Without `--install` the exe is left in `build\win\ss2_native.exe` and you copy it over yourself. On Linux or macOS the same
command works with `python3 host/build_win.py /path/to/SS2.exe` (you can only build it there, not run it).

The exe writes `ss2_native.log` next to itself. In normal use the log is short; if something goes wrong the crash report
(last recompiled functions, last native calls, stack, loaded modules) is written to it automatically.

### Switch files

Empty text files placed next to the exe change its behaviour:

| File | Effect |
| --- | --- |
| `darkrecomp_debug.txt` | verbose diagnostics: per-frame draw statistics, call tracing, heartbeat, and a few backbuffer screenshots (`darkrecomp_*.bmp`) |
| `darkrecomp_windowed.txt` | force windowed mode |
| `darkrecomp_nomsaa.txt` | force multisampling off |
| `darkrecomp_novsync.txt` | present without vsync |
| `darkrecomp_nolgvid.txt` | hide the video decoder (skips cutscenes) |
| `darkrecomp_native_osm.txt` | use the original `allobjs.osm` and `Squirrel.osm` instead of the recompiled ones |
| `darkrecomp_heapcheck.txt` | validate all heaps after every native call (slow; for tracking corruption) |
| `darkrecomp_realquery.txt` | use the real D3D frame-limiter query instead of the shortcut |

## How it works (short)

- **Flat guest memory, identity-mapped.** Guest address `a` is host address `a`. The game's data structures are exactly
  where the original code expects them, which keeps native Windows DLLs (D3D9, lgvid, scripts) and recompiled code
  interoperable.
- **Two directions of calls.** Recompiled code calling a Windows API copies the guest stack arguments to the real stack
  and measures how many bytes the callee popped. Native code calling back into the game (callbacks, scripts) enters a
  recompiled function through a dispatcher on a per-thread guest stack.
- **CPU model.** Registers live in a struct; arithmetic flags are C locals that the compiler folds away; x87 is modelled
  with `double`. MSVC's x87 intrinsics are emulated host-side.
- **Self-modifying code.** The software renderer patches its own instructions; the lifter maps every such write to the
  exact instruction field, so the game's own patching keeps working.
- **Verification.** Every function and every distinct instruction encoding was differential-tested against an x86
  emulator (numbers below, in the technical notes).

## Known issues

- **Cutscenes use a workaround.** The original `ffmpeg.dll` frees a handful of invalid pointers while opening a video.
  On the original binary this is tolerated; here the process heap aborts the program. The host redirects ffmpeg's
  allocator and skips frees of pointers that aren't valid heap blocks (a few small leaks per session). The root cause
  isn't understood yet.
- The game's log (`SS2.log`) shows `Failed to load script module ...` lines for `baseelev.osm`, `traps.osm` and one
  with an unreadable name (`+x?A.osm`, error 126). All three appear in logs from the retail game too: they are harmless
  leftovers in the engine's default script list and are safely skipped.
- `lgvid.dll`, `fmsel.dll` and `darkdlgs.dll` still run as the original DLLs, so this is not yet a fully recompiled
  program.
- C++ exceptions and `longjmp` inside recompiled code are not supported. The Squirrel compiler uses `longjmp` to report
  syntax errors, so a script with a syntax error will stop the game instead of logging the error.
- The engine's SSE code paths are disabled (the recompiler doesn't translate SSE), so it uses its x87 fallbacks, as it
  would on an old CPU.
- Windows only (the host is a 32-bit Windows executable; 64-bit Windows 10/11 run it fine). Only tested with NewDark
  2.48 on an NVIDIA GPU.
- Many diagnostic hooks from the bring-up are still in `host/win_host.c`; they are inactive unless
  `darkrecomp_debug.txt` exists.

## Names for the recompiled code (symbols)

The generated C names every function by its original address (`nd_00601430`). The build also harvests names from your
own `SS2.exe` (`tools/annotate.py`, about 20 seconds):

- **RTTI:** the engine is C++ with runtime type information, so every polymorphic class and its vtable can be found;
  each virtual method becomes `Class::vfN` (COM-style classes get `QueryInterface`/`AddRef`/`Release`).
- **Constructors/destructors:** functions that store a class's vtable into an object.
- **Import thunks** and the **string literals** each function uses (log messages, config variable names), as notes.

That names about a third of the roughly 21,000 functions automatically. `symbols/manual.sym` holds names found by hand
(the render camera, the movie I/O callbacks, the SIMD detection, ...) and overrides the generated ones. Crash reports in
`ss2_native.log` print these names. `--named-sources` also writes `out/nd_named/`: a copy of the generated C with each
function's name and notes above it, plus `functions.txt`, an index, for reading.

The generated names stay on your machine; only `symbols/manual.sym` (our own findings, keyed by address) is in the
repository. Contributions to it are welcome: one line per function, `0x<address> func <name>  # what it does`.

## Roadmap

1. Shakedown on more machines (AMD and Intel GPUs, other Windows versions) and with popular mods.
2. More names: globals and structure layouts, and hand-named functions for the main systems (render, input, physics,
   AI, save/load), in `symbols/manual.sym`.
3. Recompile the remaining Looking Glass modules (`lgvid.dll`, `fmsel.dll`, `darkdlgs.dll`) and replace
   the bundled `ffmpeg.dll` with a modern open-source decoder.
4. A platform layer (graphics, audio, input, Windows API) so the recompiled game can run beyond 32-bit Windows.

## Technical notes: the recompiler

### Layout

```
lift.py               PE -> C: function discovery, CFG, C emission, jump tables, import/indirect dispatch,
                      self-modifying-code analysis (--smc), interprocedural flags analysis
runtime/rt.h          guest CPU state, guest memory access, x87 helpers, call/return macros
harness/hx.c          test host: runs one recompiled function on a given state (faults -> outcomes)
harness/difftest.py   function-level differential test vs Unicorn running the original x86
harness/insnfuzz.py   per-instruction differential fuzzer (every distinct encoding in both binaries)
harness/smc_test.py   targeted test for self-modifying renderer code (live patch values + renderer globals)
harness/tracecmp.py   lockstep trace: first instruction where C and Unicorn diverge (needs a trace build)
build.sh              build generated C + harness into a shared library
build_trace.sh        same, with per-instruction state tracing (RT_TRACE)
```

### Recompiler usage

```sh
python3 lift.py --smc SS2.exe ss out/ss2                  # ~2 min: 16,395 functions -> 111 C files
./build.sh out/ss2 build/ss build/libss.so ss             # ~3 min
python3 lift.py --smc allobjs.osm ao out/allobjs          # ~10 s: 3,716 functions -> 26 C files
./build.sh out/allobjs build/ao build/libao.so ao

python3 harness/difftest.py SS2.exe out/ss2/ss_meta.json build/libss.so --trials 4 --report ss.json
SMC_SMALL=1 SMC_PREPATCH=1 python3 harness/smc_test.py SS2.exe out/ss2/ss_meta.json build/libss.so out/ss2
python3 harness/insnfuzz.py gen out/fz allobjs.osm SS2.exe && ./build.sh out/fz build/fz build/libfz.so fz
python3 harness/insnfuzz.py run out/fz build/libfz.so --trials 24

./build_trace.sh out/ss2 build/sst build/libsst.so ss
python3 harness/tracecmp.py SS2.exe out/ss2/ss_meta.json build/libsst.so 0x<function> <trial>
```

Needs python3, pefile, capstone, unicorn and gcc/clang. Set `UC_IGNORE_REG_BREAK=1` to silence a Unicorn
deprecation warning.

### Translation model

- Guest memory is flat: guest address `a` lives at host `M + a`. Registers live in `CPU`; arithmetic flags are C
  locals per function, so dead ones fold away. x87 is modelled with `double`, matching MSVC's default 53-bit
  precision.
- Each guest function becomes `void <prefix>_<va>(CPU *c)`. `call` pushes the real return address and calls the C
  function; `ret` pops and returns. Tail jumps become tail calls.
- Jump tables (including negative-index and gapped MSVC `memcpy` tables) become C `switch`. Other indirect
  calls go through a sorted address table, falling back to `rt_call_external`.
- Imports call `rt_call_import(c, idx)`. IAT slots hold magic addresses, so `mov reg, [IAT]; call reg` resolves too.
- **Flags across calls.** A whole-program fixpoint finds functions that read flags on entry or return results in
  flags (the CRT float helpers return NaN/Inf in ZF). Spills and reloads are emitted only at those boundaries.
  Indirect calls are treated as flag boundaries.
- **Self-modifying code.** Every absolute write into code is mapped to the exact instruction field it patches.
  SS2.exe has 237 such fields: 4-byte displacements (texture addresses), 4-byte immediates (masks), 1-byte
  immediates (shift counts) and one patched `jg` displacement. A patched instruction reads that field from guest
  memory at runtime, so the game's own patch writes need no translation. The patched branch becomes a `switch`
  over every value the code stores there. Ambiguous overlapping decodes are resolved by linear decode from the
  nearest real function start.
- `fs:` addressing uses `c->fs_base` (TIB; `fs:[0]` = SEH chain).
- Build modes: `RT_SHADOW` verifies return addresses with a shadow stack, `RT_BUDGET` bounds execution,
  `RT_TRACE` records state before every instruction.

### Verification status

**SS2.exe**, function level (16,395 functions x 4 random states; registers, x87 stack and all writable memory,
code included, compared on completion and at the faulting instruction):
- 7,421 functions match on completed runs and 8,845 match exactly at the fault point.
- 51 contain only junk decodes, 39 only reach identical faults, and 7 always exhaust the step budget.
- All 8 remaining failures are explained:
  - 80-bit vs double exponent range (3)
  - NaN sign (1)
  - dead faulting loads removed by GCC (3; verified identical in the trace build)
  - a junk decode (1)
- 10 functions differ only in state at the faulting instruction; lockstep traces agree up to the fault in every case.
- 14 functions write through random pointers into code. Sampled cases are generic copy/clear loops fed pointers
  from data, not self-modification.

**Self-modifying renderer** (20 functions that write to or contain patched code; patch fields and renderer globals
pre-loaded with live values, 48 runs each):
- 459 passing runs executed patched instructions with live values, covering 18 of the 20 functions.
- There are no plain mismatches. All 48 runs of the hardest case agree instruction for instruction in lockstep traces.

**allobjs.osm** (3,716 functions x 8): 1,995 match on completion, 1,711 at the fault point, 5 contain only junk
decodes. The 2 failures are a NaN payload and an `fxam` on an empty x87 register.

**Instruction level** (12,892 stubs / 6,414 shapes from both binaries, 24 states each):
- 308,039 of 308,049 comparable trials pass.
- The 3 failing shapes are a Unicorn bug (`fyl2x` with input <= 0) and subnormal `fmul`.

**Negative control:** a planted `sar`-as-`shr` bug is caught in 13 of 13 `sar` shapes.

### Bugs found by the verification (all fixed)

- Flags returned across `ret`/`call` by CRT assembly. Fixed by the interprocedural flags analysis.
- MSVC `memcpy` jump tables with negative indices and an unused first slot.
- x87 `fsin`/`fcos`/`fptan`/`fsincos` with |x| >= 2^63 must leave the operand unchanged and set C2.
- Pointer scanning of code sections in relocation-less images created thousands of fake function entries,
  including at every patch target.
- Unicorn harness: leftover emulator state across faulting runs, 16-bit stack segment after GDT setup.

### Known gaps

- x87 tag word (empty registers) isn't modelled; `fxam` on an empty slot differs (CRT math-error path only).
- AF flag isn't modelled (no BCD instructions in reached code; `pushfd` reports AF=0).
- x87 precision control is ignored (always 53-bit). Direct3D sets 24-bit on device creation, so renderer math
  will be slightly *more* precise than on Windows. NaN payloads, subnormals and the 80-bit exponent range differ.
- At a fault, multi-step instructions aren't atomic, and GCC may drop or reorder faulting dead loads.
  This only matters for code that is crashing anyway.
- Return addresses don't steer control flow, so setjmp/longjmp and C++ exception unwinding need explicit runtime
  support.
- The mapper variant at 0x52e100-0x52e650 has no references in the exe and is treated as dead. If it's ever
  called, the dispatch fails loudly.
- No optimisation pass yet: flag locals, registers resident in the struct, memcpy-based memory access.
