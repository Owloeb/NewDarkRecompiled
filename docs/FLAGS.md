# Every flag, switch and setting

One list of every option the project has, grouped by where you type it. Defaults are in brackets. When an option is
experimental or for debugging only, it says so. If you add an option, add it here too.

- [Building](#building): [Windows exe](#windows-exe-hostbuild_winpy), [portable host](#portable-host-portbuildpy),
  [CMake](#cmake-options-portable-host)
- [Running](#running): [`ss2port` options](#ss2port-command-line), [environment variables](#ss2port-environment-variables),
  [Windows switch files](#windows-exe-switch-files)
- [PS Vita](#ps-vita)
- [Developer tools](#developer-tools): [lifter](#lifter-liftpy), [tests](#conformance-tests-porttestsrun_conformancepy),
  [tools](#tools), [compile-time defines](#compile-time-defines-runtimerth)

## Building

### Windows exe (`host/build_win.py`)

`python host\build_win.py "<path to SS2.exe>" [options]` builds `ss2_native.exe`, which runs on Windows only.

| Option | What it does |
| --- | --- |
| `--install` | Copy the finished exe into the folder that contains `SS2.exe`. |
| `--out PATH` | Where to write the exe [`build/win/ss2_native.exe`]. |
| `--jobs N` | Parallel compile jobs [all CPU cores]. |
| `--no-osm` | Don't recompile the game's DLLs (`allobjs.osm`, `Squirrel.osm`, `lgvid.dll`, `fmsel.dll`); the originals are used. |
| `--hooks FILE` | Mods: call your own C functions at the entry of recompiled functions (see `tools/apply_hooks.py`). |
| `--extra-src FILE...` | Mods: extra C files to compile and link in (they see `runtime/rt.h`). |
| `--named-sources` | Also write `out/nd_named/`: the generated C with function names and notes, for reading. |

### Portable host (`port/build.py`)

`python3 port/build.py "<path to SS2.exe>" [options]` lifts the game to C (`out/port`) and builds `build/port/ss2port`.

| Option | What it does |
| --- | --- |
| `--backend sdl2\|null` | `sdl2` [default]: window, input, sound, OpenGL. `null`: headless (no window, sound or GL), used by the tests and when you only need the lifted C, e.g. for the Vita. |
| `--target linux\|windows` | `linux` [default]: this machine (any POSIX system). `windows`: cross-compile a 64-bit `ss2port.exe` with Zig (a test target, not for playing). |
| `--jobs N` | Parallel compile jobs [all CPU cores]. |
| `--build DIR` | Build directory [`build/port`, or `build/port-win` for `--target windows`]. |
| `--install` | Copy the result next to `SS2.exe`. |
| `--cache-regs` | **Experimental.** Lift with `lift.py --cache-regs`. Normally guest registers live in a struct and every guest memory store forces the C compiler to reload them. This keeps each function's registers in C locals (written back only around calls, faults and returns) and routes indirect calls through a small target cache instead of a binary search. Expected effect: Vita about +20% frame rate (mostly the call cache); x86-64 about +5%; 32-bit Windows hosts probably the best case but unmeasured; ARM under qemu was ~7% *slower* (qemu isn't a reliable speed guide). Passes the full conformance suite (plain and cache-regs) on Linux and on a Windows (`--target windows`) run, sanitizers clean; never measured in the real game on a PC. Switching it on or off relifts everything (~8 minutes). |
| `--low-dll-bases` | Lift the game's DLLs for load addresses below 32 MB. Only for hosts with a small guest address space (the PS Vita). |

Lifting is skipped when nothing it depends on changed. Don't interrupt it: it deletes the old output first.

### CMake options (portable host)

For building by hand: `cmake -S port -B build/port -DPORT_GENERATED=out/port [-D...]`.

| Option | What it does |
| --- | --- |
| `PORT_GENERATED` | **Required.** The lifted output directory (`out/port`). |
| `PORT_BACKEND` | `sdl2` [default], `null`, or `vita` (needs the vitasdk toolchain file). |
| `PORT_SYSTEM` | System layer: `posix` [default off Windows] or `win32`. |
| `PORT_PREEMPT` | [ON] Let other guest threads run at loop heads. The game spin-waits, so leave it on. |
| `PORT_VIDEO` | [ON] Built-in cutscene decoder (AVI, Indeo 5, PCM; LGPL code from `video/`). OFF: no cutscenes. |
| `PORT_PROF` | [OFF] **Profiling build.** Every 20 s the log lists the 30 guest functions and host imports that took the most time (`[port] PROF 12.3% nd_0068bff0`). Summarise with `tools/fnprof.py`. Slightly slower. |
| `CMAKE_BUILD_TYPE` | [RelWithDebInfo] Usual CMake meaning. |

## Running

### `ss2port` command line

`ss2port [options] <path to SS2.exe> [arguments for the game]`. Options go **before** `SS2.exe`; everything after it is
passed to the game.

| Option | What it does |
| --- | --- |
| `--windowed` | Force a window. |
| `--frames N` | Stop after N presented frames (testing). |
| `--guest-space MB` | Guest address space to reserve [4096 on 64-bit hosts]. |
| `--guest-backed MB` | Only this much of the guest space is real memory; the rest serves oversized memory pools [all]. For hosts without lazy memory commit (the Vita uses 128). |
| `--verbose` | More logging. |
| `--trace` | Record calls into the host (the first 8192 and the last 4096), printed on exit, crash or Ctrl-C. Implies `--verbose`. |
| `--trace-only a,b,...` | Record only these imports (e.g. `fopen,fread,fseek`). |
| `--list-missing` | List the game's imports the host does not implement, then exit. |
| `--list-shims` | List every implemented import, then exit. |

### `ss2port` environment variables

SDL2 backend unless noted.

| Variable | What it does |
| --- | --- |
| `SS2PORT_FULLSCREEN=0\|1` | Override the game's fullscreen choice. |
| `SS2PORT_GLES=1` | Use OpenGL ES 2.0 instead of OpenGL 2.1. |
| `SS2PORT_NOVSYNC` | Present without vsync. |
| `SS2PORT_NOSOUND` | No audio device. |
| `SS2PORT_NOALERT` | Log message boxes instead of showing them. |
| `SS2PORT_WRITE_DIR=DIR` | Write saves and config here, so the game folder can stay read-only. |
| `SS2PORT_MOUSE_SCALE=F` | Multiply mouse motion, e.g. `0.5`. |
| `SS2PORT_MOUSE_WARP=1` | Capture the mouse by re-centring the pointer, for systems where pointer capture doesn't work. |
| `SS2PORT_MOUSE_DEBUG=1` | Log raw mouse motion and the cursor position the game reads and sets. |

### Windows exe switch files

Empty text files next to `ss2_native.exe`; the name is the switch.

| File | What it does |
| --- | --- |
| `darkrecomp_windowed.txt` | Force windowed mode. |
| `darkrecomp_novsync.txt` | Present without vsync. |
| `darkrecomp_nomsaa.txt` | Force multisampling off. |
| `darkrecomp_nolgvid.txt` | Hide the video decoder (skips cutscenes). |
| `darkrecomp_native_osm.txt` | Use the original `allobjs.osm`, `Squirrel.osm`, `lgvid.dll`, `fmsel.dll` instead of the recompiled ones. |
| `darkrecomp_native_ffmpeg.txt` | Decode cutscenes with the original `ffmpeg.dll` instead of the built-in decoder. |
| `darkrecomp_debug.txt` | Verbose diagnostics: per-frame draw statistics, call tracing, heartbeat, a few backbuffer screenshots. |
| `darkrecomp_apistats.txt` | Record which Direct3D / DirectSound / DirectInput calls the game makes into `darkrecomp_api_usage.txt`. |
| `darkrecomp_heapcheck.txt` | Validate all heaps after every native call (slow; for tracking memory corruption). |
| `darkrecomp_realquery.txt` | Use the real Direct3D frame-limiter query instead of the shortcut. |

## PS Vita

Shelved (plays at about 6 to 20 fps); see [`port/backends/vita/README.md`](../port/backends/vita/README.md) and
[`PERF.md`](../port/backends/vita/PERF.md). Lift with `port/build.py ... --backend null --low-dll-bases [--cache-regs]`,
then build with CMake `-DPORT_BACKEND=vita` and the vitasdk toolchain file.

**`ux0:data/ss2/ss2port.txt`** (optional; options separated by spaces or new lines; the Vita has no environment variables). Any `ss2port` option above, plus:

| Option | What it does |
| --- | --- |
| `--swap-sticks` | Move with the right stick, look with the left. |
| `--look-speed N` | Look speed in percent [100]. |
| `--no-watchdog` | Turn off the hang watchdog (which forces a core dump when the game neither draws nor touches a file for 90 s). |
| `--no-readahead` | Turn off read-ahead for files read straight through. |

Without `--guest-space`/`--guest-backed` in that file the Vita uses 208 and 128.

**CMake cache variables:** `VITA_APP_NAME` ["System Shock 2"], `VITA_TITLEID` [`SSHK00002`], `VITA_VERSION` [`01.00`].

**Compile-time constants** (edit the source): in `plat_vita.c`: `VITA_HEAP_MB` (40), `VITA_VGL_RAM_MB` (6),
`VITA_GUEST_SPACE_MB_STR` ("208"), `VITA_GUEST_BACKED_MB_STR` ("128"), `VITA_WATCHDOG_S` (90), `VITA_GAME_DIR`
(`ux0:data/ss2`); in `plat_vita_sys.c`: `VITA_RDBUF` / `VITA_WRBUF` (64 KB read window and write buffer),
`VITA_MEMFILE_MAX` (32 MB: files created for writing stay in memory up to this size), `VITA_MAXFILES` (128).

## Developer tools

### Lifter (`lift.py`)

`python3 lift.py <exe or dll> <prefix> <outdir> [options]`. The build scripts call it for you.

| Option | What it does |
| --- | --- |
| `--smc` | Analyse and translate self-modifying code (used for `SS2.exe`). |
| `--rebase ADDR` | Lift a DLL as if loaded at this address (applies its relocations first). |
| `--iat-indirect` | For DLLs loaded by Windows: call imports through their IAT slot as plain indirect calls. |
| `--cache-regs` | **Experimental.** Guest registers in C locals per function, indirect calls through a target cache (needs `runtime/rt_fast.h`). Default output is unchanged without it. |
| `--funcs-per-file N` | Functions per generated C file [150]. |

### Conformance tests (`port/tests/run_conformance.py`)

Builds a game-free 32-bit test program, lifts it and runs it on the host. No game files needed.

| Option | What it does |
| --- | --- |
| `--backend null\|sdl2` | Backend to test [`null`]; `sdl2` adds pixel-exact rendering checks (needs a display, e.g. `--wrap "xvfb-run -a"`). |
| `--target native\|windows` | `windows`: build the Windows exe (run it with `--wrap wine` or on Windows). |
| `--cache-regs` | Lift the test program with `--cache-regs`. |
| `--cflags "..."` | Extra C flags, e.g. `-fsanitize=address,undefined` or `-DRT_PROF`. |
| `--cc COMPILER` | C compiler to use. |
| `--config TYPE` | CMake build type [RelWithDebInfo]. |
| `--wrap "CMD"` | Run the host under this command (`valgrind -q`, `xvfb-run -a`, `wine`). |
| `--build DIR` | Build directory [`build/conformance`]. |
| `--skip NAME` | Skip a check (repeatable). |
| `--no-rebuild-guest` | Reuse the previously lifted test program. |

### Tools

| Command | What it does |
| --- | --- |
| `tools/annotate.py <SS2.exe> <out/nd> <out.sym>` | Harvest function names and notes from the exe (RTTI, constructors, imports, strings). |
| `tools/name_sources.py <out/nd> <outdir> <file.sym>...` | Write a readable copy of the generated C with names and notes. |
| `tools/apply_hooks.py [--hooks FILE] <lifted dir>...` | Insert calls to your own C functions at recompiled function entries (mods). |
| `tools/where.py <exe> <addr> [before] [after]` | Disassemble around a guest address (e.g. from a crash report); `-s` prints the string there. |
| `tools/fnprof.py <exe> <log> [skip] [top]` | Average the `PROF` tables of a `PORT_PROF` build and describe the hottest guest functions. |
| `port/tools/check_shims.py`, `check_com.py` | Check every shim's and COM method's stack-pop count against the MinGW headers. |
| `port/tools/gen_ifaces.py` | Generate the COM interface tables. |

### Compile-time defines (`runtime/rt.h`)

Set by the build scripts; listed so the generated code can be read. You normally don't touch these.

| Define | What it does | Set by |
| --- | --- | --- |
| `RT_PREEMPT` | Loop heads check whether another guest thread is waiting. | CMake `PORT_PREEMPT` |
| `RT_PROF` | Function entries and loop heads record their name for the profiler. | CMake `PORT_PROF` |
| `RT_IDENTITY` | Guest address = host address (32-bit Windows exe). | `build_win.py` |
| `RT_RING` | Remember the last guest functions entered, for crash reports. | `build_win.py` |
| `RT_BUDGET` | Count down at loop heads and hand control back to the harness. | `build.sh` (test harness) |
| `RT_SHADOW` | Check every return address against a shadow stack. | `build.sh` (test harness) |
| `RT_TRACE` | Call `rt_trace` at every instruction (very slow). | `build_trace.sh` |
