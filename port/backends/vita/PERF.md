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

## Results log (same save, busy area, --cache-regs + PORT_PROF build; fps = 5 s windows in play, loading excluded)
- Before the indirect-call cache: mean 7.9 fps (range 4.6-13.9). After (a94675e): mean 9.6 fps (range 4.8-14.1). About +20%, but the
  walking route differed, so treat as roughly "a bit better", not exact.
- The CRT `fread` wrapper is still the top entry (6-16% of samples; higher in windows with 110+ reads per 5 s): about 7 ms per read
  of ~32 KB, on the game thread. File reads in play are a real cost, separate from the CPU work.
- Loading: one loader function takes 44% of a 20 s window; the level load itself is now fast.

## Ideas, in rough order of expected value
1. (done, see results) indirect-call cache.
1b. Read-ahead for file reads during play (a prefetch thread or bigger window) to take the 6-16% file wait off the game thread.
2. Inline tiny functions (lifter, or LTO / fewer files for the generated code).
3. x87 `top` tracking at lift time (st[] as locals); callee-saved register elision.
4. Reduce `fread` cost (read-ahead / larger window for the streams the game reads during play).
5. Lower in-game settings (resolution, view distance, sound channels), skip cutscenes, cache negative `stat` probes at startup.
6. Compiler experiments: -O2/-Os, ARM instead of Thumb (low expected value).

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
