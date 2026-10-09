# Vita performance notes

Where the port stands and what has been tried, so work can resume without the conversation history. Everything here is about the
`vita-backend` branch; `main` is not affected by any of it (all Vita-specific or experimental pieces are off by default).

## Status
- Level transitions: fixed. The level-exit save wrote the level file in thousands of tiny `sceIoWrite` + seek calls and the
  watchdog did not count writes as progress. Files created for writing now stay in memory until close (`plat_vita_sys.c`), and the
  watchdog counts any file activity.
- Frame rate in play is about 5-20 fps (scene dependent; busy areas with 50-110 draws per frame are the slowest). The game thread
  is 85-97% busy; rendering is 3-11%. The limit is the CPU cost of the recompiled code.

## Tools
- `lift.py --cache-regs` (`port/build.py --cache-regs`): guest registers in C locals per function, indirect calls through a small
  recent-target cache. Experimental, off by default. Gain so far: "feels a bit better".
- `cmake -DPORT_PROF=ON`: sampling profiler. Functions and loop heads store their name; a thread samples it every ~1 ms and logs the
  top 30 every 20 s as `PROF xx.x% nd_XXXXXXXX` lines in `ss2port.log`. Time is charged to the last function entered, and host
  work in imports (file reads) is charged to the caller.
- `tools/fnprof.py <exe> <ss2port.log> [skip] [top]`: averages the tables and describes each hot function (size, x87 ops, loops,
  calls). Needs the game exe and `pip install pefile capstone`.
- Symbols: `arm-vita-eabi-nm -n -S build/vita/ss2port > ~/ss2port.syms.txt` (must be from the same build as the log or dump).
- Build order: `python3 port/build.py <exe> --backend null [--cache-regs]` lifts into `out/port` (do not interrupt it: it deletes
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

## Ideas, in rough order of expected value
1. Measure the indirect-call cache (relift with `--cache-regs`, same area, compare fps and the profile).
2. Inline tiny functions (lifter, or LTO / fewer files for the generated code).
3. x87 `top` tracking at lift time (st[] as locals); callee-saved register elision.
4. Reduce `fread` cost (read-ahead / larger window for the streams the game reads during play).
5. Lower in-game settings (resolution, view distance, sound channels), skip cutscenes, cache negative `stat` probes at startup.
6. Compiler experiments: -O2/-Os, ARM instead of Thumb (low expected value).
