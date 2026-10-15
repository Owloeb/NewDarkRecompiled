# port/: the portable host

The recompiled game is plain C that only touches memory (`M + address`) and calls out for everything the operating
system used to do. This directory is the other half: a host that runs it **without Windows and without x86**. A port to a
new platform only writes the small bottom layer.

**Contents:** [Run on Linux](#run-on-linux) · [Options](#options) · [Build details](#build-details) ·
[Port to a new platform](#port-to-a-new-platform) · [Architecture](#architecture) · [Verification](#verification) ·
[Status and limitations](#status-and-limitations)

## Run on Linux

For Ubuntu, Debian and WSL. You need your own NewDark 2.48 `SS2.exe`; the generated C is built on your machine and never
distributed.

```sh
# 1. tools and libraries
sudo apt update
sudo apt install -y git build-essential cmake ninja-build python3-venv libsdl2-dev

# 2. code
git clone https://github.com/Owloeb/NewDarkRecompiled.git
cd NewDarkRecompiled

# 3. Python environment for the lifter (use `source`; do not execute the activate script)
python3 -m venv .venv
source .venv/bin/activate
pip install pefile capstone

# 4. lift and build: about 8 minutes the first time, unchanged steps are skipped afterwards
python3 port/build.py "/path/to/System Shock 2/SS2.exe"
```

The result is `build/port/ss2port`. Run it from the game folder. **Options go before the exe**; anything after
`SS2.exe` is passed to the game.

```sh
cd "/path/to/System Shock 2"
~/NewDarkRecompiled/build/port/ss2port --windowed SS2.exe
```

With no exe argument it uses `SS2.exe` in the current folder or next to `ss2port`. `build.py --install` copies `ss2port`
next to `SS2.exe`, so `./ss2port --windowed` works from there.

**Tips**
- **WSL:** keep the game on the Linux filesystem (copy it from `/mnt/c/...` to e.g. `~/ss2`); every file access through
  `/mnt/c` is slow. (Writes are buffered, so level changes are fast even there, but loading still suffers.) A window and sound need WSLg (Windows 11, or Windows 10 with a recent WSL).
- **Mouse:** WSLg cannot capture the pointer, so mouselook is unreliable there. On native Linux (X11/Wayland) SDL2 uses
  relative mouse mode (untested so far). If motion is too fast or slow, set `SS2PORT_MOUSE_SCALE` (e.g. `0.5`).
- **Saves and config** are written next to the game; set `SS2PORT_WRITE_DIR` to keep the game folder read-only.
- **Problems:** run with `--verbose`. For a crash or hang add `--trace` (bounded; dumped on crash, Ctrl-C or exit).

## Options

The complete list of every option in the project is in [`docs/FLAGS.md`](../docs/FLAGS.md).

Command line, before the exe:

| Option | Effect |
| --- | --- |
| `--windowed` | run in a window |
| `--verbose` | more logging |
| `--trace`, `--trace-only a,b,name*` | record the first 8192 and last 4096 calls (with return values and callers), optionally only the named ones |
| `--frames N` | exit after N frames (tests) |
| `--guest-space MB` | guest address space to reserve (default 4096 MB on 64-bit hosts; 32-bit hosts need a smaller one) |
| `--list-missing`, `--list-shims` | list Windows/DirectX functions the game imports that are not implemented, or all that are |

Environment variables are set before you start `ss2port`:

```sh
# Linux / WSL: one run (or `export` it for the session)
SS2PORT_NOVSYNC=1 ./ss2port --windowed SS2.exe

# PowerShell: stays set in that window
$env:SS2PORT_NOVSYNC = "1"; .\ss2port.exe SS2.exe
```

Use `1` for on/off switches. Command Prompt: `set SS2PORT_NOVSYNC=1`.
| Variable | Effect |
| --- | --- |
| `SS2PORT_FULLSCREEN` | start fullscreen |
| `SS2PORT_GLES` | force OpenGL ES instead of OpenGL 2.1 |
| `SS2PORT_NOVSYNC` | present without vsync |
| `SS2PORT_RENDER_THREAD=1` | run the renderer on a thread of its own (the game computes the next frame while it draws this one) |
| `SS2PORT_NOSOUND` | no audio device |
| `SS2PORT_NOALERT` | log message boxes instead of showing them |
| `SS2PORT_WRITE_DIR` | where saves and configuration are written (the game folder stays read-only) |
| `SS2PORT_MOUSE_SCALE` | multiply mouse motion, e.g. `0.5` |
| `SS2PORT_MOUSE_WARP=1` | capture the mouse by re-centring the pointer, for systems where pointer capture does not work (known to feel bad under WSLg) |
| `SS2PORT_MOUSE_DEBUG=1` | log raw mouse motion and the cursor position the game reads and sets |

## Build details

`port/build.py` lifts `SS2.exe` and its DLLs to C, then builds with CMake:

```sh
python3 port/build.py "/path/to/SS2.exe" [--backend sdl2|null] [--jobs N] [--target linux|windows] [--install] [--cache-regs] [--fastpaths] [--abi-check] [--prof]
# by hand, from already lifted output:
cmake -S port -B build/port -DPORT_GENERATED=out/port [-DPORT_BACKEND=sdl2|null]
cmake --build build/port
```

- **Compiler:** GCC or Clang (the generated code uses `__builtin_setjmp`).
- **CMake options:** `PORT_PREEMPT` (default ON: other guest threads may run at loop heads, needed for games that
  spin-wait), `PORT_VIDEO` (default ON: built-in Indeo 5 cutscene decoder, LGPL code from `video/`),
  `PORT_BACKEND`, `PORT_SYSTEM` (`posix` or `win32`).
- **Speed: `build.py --cache-regs`** (opt-in; the default lifted code is unchanged; switching relifts everything, ~8 min).
  Guest registers live in C locals inside each function instead of the CPU struct. Around a call only what the callee may
  change is written back and reloaded: indirect calls and imports are held to the calling convention, and `lift_preserve.py`
  proves which callee-saved registers each function restores. `lift_x87.py` keeps the x87 stack in C locals too. Indirect
  calls go through a per-thread cache of recent targets. Details in [docs/RECOMPILER.md](../docs/RECOMPILER.md#register-caching---cache-regs).
  Played on Linux and on the PS Vita with the run-time checks below and no violations.
- **`--abi-check`** (with `--cache-regs`): every indirect call and import checks the convention the lifter relies on and
  logs `ABI VIOLATION: ...` once per call site (and corrects it). For a test session; leave it off for timing.
  `--no-x87`, `--no-preserve` and `--no-abi-calls` switch off one optimisation each, for bisecting a problem.
- **Fast paths: `build.py --fastpaths`** swaps the hottest engine functions for native C with identical results
  (`core/fastpaths.c`): the string-name hash, the engine's integer and byte-string hashes and its two hash-table lookups
  (hot in combat: AI property and link queries), an 8-byte key compare, `_ftol` and `_ftol2`, and the 2D bitmap copy. Each
  declines (and the recompiled original runs) for input it doesn't model, such as a division by zero. No re-lift needed.
  `tests/fastpaths/check.py` checks each one against the game's own instructions in an x86 emulator.
- **Render thread:** `backends/sdl2/gl_thread.c` can run the whole renderer on another core. The game thread copies each
  call (vertices, indices, texture data) into a 4 MB ring and carries on; the render thread replays it, at most one frame
  behind. The Direct3D front end also hands it the per-vertex work (transform, lighting, fog, texture coordinates) through
  `plat_gfx_deferred`. On by default on the Vita; `SS2PORT_RENDER_THREAD=1` on the desktop.
- **Profiling: `build.py --prof`** (CMake `-DPORT_PROF=ON`) builds a sampling profiler (`core/prof.c`): every 20 s the log
  lists the 30 guest functions and host imports that took the most time (`[port] PROF 12.3% nd_0068bff0`).
  `tools/fnprof.py <exe> <log>` averages those tables and describes the hottest guest functions. The sample is one global
  "current function", so with several busy guest threads the table mixes them (a background thread's `fread` shows up too).
- **Windows target** (`--target windows`): cross-compiles a 64-bit `ss2port.exe` from Linux or WSL with Zig
  (`pip install ziglang`; SDL2 is fetched and checksum-verified). In the game folder run `.\ss2port.exe --windowed SS2.exe`.
  It exercises the shared code on a real Windows mouse, keyboard and debugger; it is a development and test target, not
  for playing (use the normal Windows build for that, see the [main README](../README.md#play-on-windows)).

## Port to a new platform

1. Copy `backends/null/` to `backends/<yours>/` and select it with `-DPORT_BACKEND=<yours>`. It builds and runs headless.
2. **System part** (`backends/posix/` is the reference): `plat_mem_*` (reserve address space, commit, discard), time,
   threads (`plat_thread_start`, mutex, condition variable) and file/directory operations. On a POSIX-like platform reuse
   it unchanged with `-DPORT_SYSTEM=posix`.
3. **Video and input:** open a window or display and deliver key, mouse and quit events as DIK scan codes in
   `plat_video_poll`.
4. **Renderer:** `plat_tex_*` and `plat_gfx_*`. You receive clip-space vertices and a fully resolved draw state (blend,
   depth, stencil, up to eight texture stages already reduced to what the front end could not do on the CPU).
   `sdl2/gl_render.c` generates GLSL from that state and is a good template for other APIs. Also implement
   `plat_gfx_deferred(fn, data, n)` (run `fn` with `draw` = your draw function; `backends/null` shows the one-line version).
   To run your renderer on its own thread, put `sdl2/gl_thread.c` in front of it as the Vita backend does.
5. **Audio:** `plat_audio_*` pulls 16-bit stereo from the shared software mixer.
6. Run `python3 port/tests/run_conformance.py --backend <yours>`; it needs no game files.

Requirements on the platform: a little-endian CPU, a C99 compiler with GCC extensions, and enough address space for the
guest (compact layout by default, configurable with `--guest-space`; 32-bit hosts work with a small guest space).

## Architecture

```
generated C (nd/, ao/, sq/, lv/, fm/, mods/)   the game, vanilla, identical on every platform
   | rt_call_import / rt_call_external
core/    address space, heap (TLSF), thunks, PE loading, guest threads, kernel objects, virtual file system
win32/   KERNEL32 / USER32 / GDI32 / ADVAPI32 / WINMM: files, time, memory, windows, messages, input mapping
crt/     MSVCR90: MSVC-exact printf/scanf, buffered stdio, string, math, qsort/bsearch/rand, std::string, exceptions stubs
dx/      Direct3D 9 (state, vertex processing, formats, render targets), DirectSound (software mixer), DirectInput 7,
         DirectDraw (detection), the built-in cutscene decoder (video.c, stands in for ffmpeg.dll)
   | include/plat.h        <- the only interface a port implements
backends/posix/   system part: memory, time, threads, files   (any POSIX system)
backends/win32/   system part for Windows (a development and test target: to play on Windows use the normal Windows build)
backends/sdl2/    window, input, audio, OpenGL 2.1 / OpenGL ES 2.0 renderer (gl_render.c), optional render thread (gl_thread.c)
backends/vita/    PS Vita: vitaGL on gl_render.c + gl_thread.c, card-friendly file layer, sound-archive cache
backends/null/    headless (everything accepted, nothing shown); used for tests
```

Everything above `plat.h` is shared and contains no operating-system calls. `plat.h` documents its own conventions:
Direct3D clip space with the half-pixel offset already applied, top-left texture origin, `0xAARRGGBB` colours, DIK scan
codes for keys.

## Verification

- `tools/check_shims.py` compares every shim's declared stack-pop count with the MinGW headers/.def files (a wrong count
  corrupts the guest stack); `tools/check_com.py` does the same for COM vtables (generated by `tools/gen_ifaces.py`).
- `tests/run_conformance.py` builds a game-free 32-bit Windows test program, lifts it with `lift.py` (the same recompiler
  as the game) and runs it on the chosen backend: 509 checks on `null`, 536 on `sdl2` (which adds pixel-exact rendering
  checks; it also passes with `SS2PORT_RENDER_THREAD=1`). It also runs clean under ASan, UBSan and TSan, and as a 32-bit build with a small guest space.
  `--cache-regs` runs the same checks on code lifted with `lift.py --cache-regs`; `--abi-check` adds the run-time convention
  checks and fails on any violation.
  `--target windows --wrap wine` builds and runs the Windows exe (under Wine, or natively on Windows).
  Verified on Windows (cross-built 64-bit exe launched from WSL, NVIDIA OpenGL 4.6): 476/476 on `null` and 503/503 on
  `sdl2`, each both plain and with `--cache-regs`.
  The `--target windows` game build (`ss2port.exe`) was also played on Windows: intro cutscene, main menu and a level all worked.
- More suites, each runnable without game files unless noted:
  - `../harness/abi_test/run.sh`: register caching end to end (contract, preservation proof, run-time check, exemptions),
    with two programs that break the rules on purpose and must be caught. `../harness/abi_test/csmith_fuzz.py [--float]`:
    random C programs (Csmith), plain lift against `--cache-regs`, results must agree.
  - `tests/glthread/run.sh`: 600,000 random renderer calls through the render thread and directly must hash identically
    (ring wrap-around, back-pressure, oversized calls, read-backs, deferred work); built with ThreadSanitizer.
  - `tests/sndcache/run.sh`: the Vita sound cache against a replayed working set; every byte checked (ThreadSanitizer).
  - `tests/fastpaths/check.py <dump>`: each fast path against the game's own code (needs a `tools/where.py` dump of the
    functions from your `SS2.exe`; nothing from the game goes into the repository).
- Four independent review passes (core, Win32/CRT, DirectX, backends and tooling) found issues that are fixed and covered
  by regression checks in the conformance suite.

## Status and limitations

- **Works:** start-up, menus, cutscenes with sound, in-game sound, saves (tested by loading Rickenbacker and Body of the
  Many), on Linux (WSL, Mesa software OpenGL) and as a 64-bit Windows build.
- **Mouselook:** verified on the Windows build. Under WSLg it cannot be judged (no pointer lock); native Linux is untried.
- **Rendering:** fixed-function vertex processing; pixel shaders up to ps_2_0 are translated to GLSL
  (`dx/d3d9_shader.c`; the game's own 16 post-processing shaders all translate, and two are checked pixel for pixel
  against hand-computed results on Mesa, desktop GL and GLES). Vertex shaders are refused (the game has none). Cube
  and volume textures are not drawn; `ProcessVertices` is not implemented.
- **PS Vita:** `backends/vita/` plays, level changes and saves included: about 15-21 fps exploring, 6-8 in firefights,
  limited by the CPU running the recompiled code. Build steps in its README, measurements and ideas in `backends/vita/PERF.md`.
- **Not done:** C++ exceptions and structured exception handling, SSE and lock-prefixed instructions in the lifter,
  `fmsel.dll` (intentionally refused), a size-optimised build for small devices.
