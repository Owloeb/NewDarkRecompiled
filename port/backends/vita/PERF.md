# Vita performance notes

**Status (2026-10-13): plays; about 15-21 fps exploring, 6-8 in firefights.** Level changes are fast, the render thread,
file layer and sound cache keep their work off the game thread, and what remains is the game's own code on core 0: about
45 ms per frame plus ~0.3 ms per draw call (it was ~64 ms + ~1 ms on 2026-10-09). See
[Results log 3](#results-log-3-2026-10-13-cache-regs--x87-pass-render-thread-fast-paths) for what each step did, and
[What did not help](#what-did-not-help) before trying the same again.

Where the port stands and what has been tried, so work can resume without the conversation history. Everything here is about the
Vita backend; nothing here changes the PC builds (all Vita-specific or experimental pieces are off by default).

## Status
- Level transitions: fixed. The level-exit save wrote the level file in thousands of tiny `sceIoWrite` + seek calls and the
  watchdog did not count writes as progress. Files created for writing now stay in memory until close (`plat_vita_sys.c`), and the
  watchdog counts any file activity.
- Frame rate in play: 15-21 fps exploring (35-70 draws per frame), 6-8 fps in firefights (110-180 draws per frame). The game
  thread is 90-100% busy; the render thread 4-9%, and the game almost never waits for it. The limit is the CPU cost of the
  recompiled code.

## Tools
- `port/build.py --cache-regs`: guest registers and the x87 stack in C locals, the calling-convention contract, the
  preservation proof, a per-thread indirect-call cache ([docs/RECOMPILER.md](../../../docs/RECOMPILER.md#register-caching---cache-regs)).
  `cmake -DPORT_ABI_CHECK=ON` (`build.py --abi-check`) checks it at run time: a checked Vita session reported no violations.
- `port/build.py --fastpaths`: native versions of the hottest engine functions (`port/core/fastpaths.c`).
- `port/build.py --prof`: the sampling profiler below, without a relift.
- `cmake -DPORT_PROF=ON`: sampling profiler. Functions and loop heads store their name; a thread samples it every ~1 ms and logs the
  top 30 every 20 s as `PROF xx.x% nd_XXXXXXXX` lines in `ss2port.log`. Time is charged to the last function entered, and host
  work in imports (file reads) is charged to the caller.
- `tools/fnprof.py <exe> <ss2port.log> [skip] [top]`: averages the tables and describes each hot function (size, x87 ops, loops,
  calls). Needs the game exe and `pip install pefile capstone`.
- Symbols: `arm-vita-eabi-nm -n -S build/vita/ss2port > ~/ss2port.syms.txt` (must be from the same build as the log or dump).
- Build order: `python3 port/build.py <exe> --backend null --low-dll-bases [--cache-regs]` lifts into `out/port` (do not interrupt it: it deletes
  the old output first), then the VPK comes from CMake with `-DPORT_BACKEND=vita` and the vitasdk toolchain file.
  `--backend null` is only the headless host used by `build.py` and the conformance suite.

## Findings (profile of a busy play area, --cache-regs build)
- The profile is flat: the top five entries are about 25% together, then hundreds of functions below 1% each. No single hot loop
  to rewrite; native C replacements of a few functions would give roughly 10% at best.
- The top entry (about 7%) is a small CRT wrapper around `fread`: this is host file-read time (the "files" figure), not guest CPU.
- Most of the other top entries are dominated by indirect calls (virtual calls, callbacks). Each used to be a binary search over
  every function (about 15 dependent loads); now a 2048-entry cache under `--cache-regs`.
- Several top entries are 3-5 instruction functions: call overhead only, because the generated C is split over many files and
  cannot be inlined.
- Loading: a few functions take 40%+ of the CPU during level loads (probably loader code), worth a look for load times.

## Results log (same save, busy area, --cache-regs + PORT_PROF build; fps = 5 s windows in play, loading excluded)
- Before the indirect-call cache: mean 7.9 fps (range 4.6-13.9). After (a94675e): mean 9.6 fps (range 4.8-14.1). About +20%, but the
  walking route differed, so treat as roughly "a bit better", not exact.
- The CRT `fread` wrapper is still the top entry (6-16% of samples; higher in windows with 110+ reads per 5 s): about 7 ms per read
  of ~32 KB, on the game thread. File reads in play are a real cost, separate from the CPU work.
- Loading: one loader function takes 44% of a 20 s window; the level load itself is now fast.

## Results log 2 (PORT_PROF with import attribution + read-ahead build)
- Time inside imports is now charged to the import. In play: `IDirect3DDevice9::DrawPrimitiveUP` 10-17% (most of it the GL draw calls
  inside it, the "render" figure), `fread` 9-13% in windows with resource loading, `Present` ~1-3%. Guest code itself stays flat.
- Draw calls per frame vary from 25 to 250 depending on the view, and frame time follows them (rough fit: ~64 ms + ~1 ms per draw).
- Read-ahead found nothing to do: the resource archives (OBJ/IFACE/SND2/BITMAP/MESH .CRF) are read at random (0-1% sequential).
  Each such read costs ~7.5 ms: the card moves ~10-12 MB/s, and a 64 KB window was fetched for ~31 KB requests, wasting half.
  Fix: a file read at random now fetches only what was asked for (16-64 KB); sequential files keep the 64 KB window and read-ahead.
- During level loads the guest spends up to ~25% of samples in `Sleep` (the game sleeping, not computing): worth finding out who sleeps.

## Results log 3 (2026-10-13: cache-regs + x87 pass, render thread, fast paths)
Same kind of sessions (Medical/Engineering, exploration and combat), PORT_PROF builds at 444 MHz.
- Lifter (`--cache-regs` grew a calling-convention contract, a preservation proof and the x87 stack in locals): exploration
  went from ~10 to 13-18 fps. A per-thread indirect-call cache fixed a race in the shared one (wrong calls under threads).
- Level changes were slow on the PC under WSL: the CRT's `fwrite` was unbuffered (thousands of tiny writes per level save).
  Streams now have a 64 KB write buffer (`crt/stdio.c`). The Vita already kept written files in memory.
- Render thread (vitaGL on core 1, then also the D3D per-vertex work through `plat_gfx_deferred`): noticeably smoother,
  but fps moved little: the render thread is only 4-9% busy. GL was never the big cost; the game's own code is.
- Sound: the game reads `SND.CRF`/`SND2.CRF` in ~32 KB pieces all the time (up to ~1,100 reads per 15 s in combat). At
  4 MB the cache missed half of them, nearly all re-reads of evicted pieces; it now grows to 24 MB (bounded by free host
  heap) and the log splits misses into re-reads and new data. These reads are on a background guest thread: the game
  thread's own file time in play is 0-1%, so sound does not cost frame rate.
- Fast paths for the combat hot spots (the engine's hash tables, an 8-byte compare, `_ftol2`): those functions left the
  profile; a few percent overall.
- Where the time goes now (combat): a flat profile across thousands of functions; float-heavy code (models, animation,
  lighting) and object queries at the top, none above ~2%. Frame time follows the number of objects drawn.

## Changes of 2026-10-20 (not yet measured on the Vita)
- **Clocks:** the port used to set the CPU to 444 MHz at start, which threw away an overclocking plugin's higher clock. Now
  it only raises clocks (`--arm-clock N` / `0` to choose or leave them; the log's `clocks:` line). The numbers above were
  all taken at 444; note the clock with every new measurement.
- **`fist`/`fistp` without libm** (`runtime/rt.h`, `F2I32`/`F2I16`): they rounded through `nearbyint`/`floor`/`ceil`/`trunc`,
  which are library calls on ARMv7 (`nearbyint` also saves and restores FPSCR). Now an inline range check and VFP conversion;
  same results in all rounding modes (`port/tests/f2i/run.sh`, 34 million cases on x86-64 and on ARM under QEMU, three
  planted bugs caught). Needs no relift, only a recompile. `fistp qword` (`F2I64`) and `frndint` still use libm.
- **`-fno-math-errno`** on the generated code: `sqrt` is one instruction without a fallback call.
- **Optimisation level of the generated code** (`PORT_GAME_OPT`, `PORT_GAME_HOT`, `tools/hotfiles.py`; README section 2).
  Expected: `-O2` or hot/cold beats `-O3` through the instruction cache. To be measured.

How to compare two builds: same save, same route, same clock, a `PORT_PROF=OFF` build, the `profile:` lines' fps and `game cpu`
over the same stretch (exploring and a firefight separately), at least two runs each. Suggested order: current build with
the new clock behaviour only (`-DPORT_GAME_OPT=-O3`, which is the old level); then `-O2`; then a hot list (profile it first);
then `-Os` everywhere as a control.

## What did not help
- **An AI efficiency mod.** `AI_Efficiency` on the `Monsters` archetype (a gamesys DML: distant AIs think less often, also
  in combat and in view) loaded fine and made enemies visibly dumber, and combat fps did not change. AI thinking is not
  the bottleneck. Don't ship it.
- **GPU settings in `cam_ext.cfg`** (multisampling, anisotropic filtering, mipmaps, distortion effects): no effect; the
  GPU is idle. The CPU-side ones (`enhanced_*_trace`, `sun_runtime_obj_shadows`) are worth a few percent.
- **Bigger sound cache for frame rate**: fixed the misses, not the frame rate (the reads are off the game thread).

## Ideas, in rough order of expected value
1. Faster generated code across the board, the only lever left that moves everything: inline tiny functions (many hot
   functions are 3-5 instructions of call overhead; the generated C is split over many files, so GCC can't inline across
   them: LTO, or the lifter inlining small leaf functions), then profile-guided optimisation or hot/cold code layout from
   the PROF tables (the instruction cache is 32 KB against tens of MB of code). Expected 1.3-1.5x, not more.
2. More fast paths where the profile shows a clear, self-contained hot function (check each with
   `port/tests/fastpaths/check.py`). Diminishing returns: the top of the profile is ~2% per function.
3. The remaining D3D front-end work on the game thread (~5%: index expansion, state snapshot, copying).
4. Fewer objects per frame in heavy scenes (engine-side: particle and debris limits), if the look can take it.
5. Compiler experiments: -O3, ARM instead of Thumb (low expected value).
Done: libm-free `fist`/`fistp`, `-fno-math-errno`, the optimisation-level options (to be measured), indirect-call cache, x87 in locals, callee-saved register elision (preservation proof), render thread, read-ahead
and sound cache, buffered writes.

## Debugging hangs and crashes (what we learned)
- The watchdog (`plat_vita.c`) fires when the game neither draws nor touches a file for 90 s. It logs what was in flight (each file
  call kind, every open file, the renderer call in progress, a guest-memory high-water mark) and then crashes on purpose so the Vita
  writes a `.psp2dmp` core dump. `--no-watchdog` in `ss2port.txt` turns it off.
- Reading a dump: the file is gzip'd, so `zcat` it first. Symbolize with
  `arm-vita-eabi-nm -n -S build/vita/ss2port > ~/ss2port.syms.txt` from the SAME build as the dump. The load address differs from
  the ELF: ELF-linked address = 0x81000000 + (runtime address - the code segment base). Convert once only; converting twice
  gives garbage names.
- The level-change hang: the level-exit save wrote the level file in thousands of tiny pieces with seeks, each a slow `sceIo`
  call, and the watchdog did not count writes as progress. Fix: files created empty for writing are kept in RAM (up to 32 MB)
  and written whole on close or flush; the watchdog counts any file activity. A 64 KB write-behind buffer alone did NOT fix it,
  and redirecting stdout to a file was a wrong guess (the vitaGL build is release and logs nothing).
- Vita file layer facts: `sceIo` calls are slow, so reads use a 64 KB window and writes a 64 KB buffer. Free user RAM is about
  30 MB after vitaGL; the guest block uses `--guest-space 208 --guest-backed 128` and its highest used page sits flat near 116 MB.
- `ss2port.txt` is optional and does not exist by default; `ss2port.log` is the log, `SS2.log` the game's own log.
