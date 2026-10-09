# NewDarkRecompiled

A static recompiler that turns the 32-bit x86 binary of **System Shock 2 (NewDark 2.48)** into portable C, with two ways
to run the result: a small **Windows host** (native `ss2_native.exe`, in place of `SS2.exe`) and a **portable host** that
runs it on Linux and, with a small backend, anywhere else.

**Status: playable on flat screen.** Boot, menus, character creation, all levels, save/load, HUD, audio, input,
fullscreen and cutscenes all run through recompiled code. Work in progress (see [Known Issues](#known-issues)).

**Jump to:** [Play on Windows](#quick-start-windows) · [Run on Linux or Elsewhere](#quick-start-linux-and-other-platforms) ·
[How It Works](#how-it-works) · [Known Issues](#known-issues) · [Build On It!](#build-on-it) ·
[Portable Host Docs](port/README.md) · [Recompiler notes](docs/RECOMPILER.md)

## Why This Exists

System Shock 2's engine source was never released, so changing the engine meant patching the binary. This project turns
the binary into C that can be rebuilt, read and changed: a **vanilla recompilation** that plays exactly like the
original, as a foundation for fixes, engine-level mods (a VR version is being built separately on top of this) and
ports. This repository stays vanilla: it reproduces the original game and adds nothing to it.

## Legal and Ground Rules

The tools are MIT-licensed (see LICENSE). The game, its engine and anything generated from it are not. The Indeo 5
decoder in `video/ffmpeg/` is unmodified FFmpeg code under LGPL 2.1 (see `video/README.md`).

- No leaked Dark Engine source was used; everything is derived from binaries by analysis and testing.
- The generated C and the built executables are derived from the copyrighted game. **They are not distributed.** This
  repository ships only the tools; you generate everything from a copy of the game you own.
- You need a legitimate install of System Shock 2 with the NewDark 2.48 executable (GOG and Steam are the usual source).
  The 25th Anniversary Remaster is a different engine and out of scope.

## Quick Start: Windows

1. Install [Python 3](https://www.python.org/downloads/) (tick "Add python.exe to PATH").
2. In PowerShell, in the folder containing `lift.py`, install the packages (the compiler, Zig, comes with `ziglang`):

        python -m pip install pefile capstone ziglang

3. Build against your own `SS2.exe` (quote the path); `--install` copies the result into that folder:

        python host\build_win.py "C:\Games\System Shock 2\SS2.exe" --install

   The first build takes about 20 minutes (8 to convert the game to C, 10 to compile); later builds redo only what
   changed. `Data\allobjs.osm`, `osm\Squirrel.osm`, `lgvid.dll` and `fmsel.dll` next to `SS2.exe` are found and
   recompiled automatically (`--no-osm` skips them).
4. Run `ss2_native.exe` from your System Shock 2 folder instead of `SS2.exe`. Leave everything else where it is (game
   data, `lgvid.dll`, `fmsel.dll`, `allobjs.osm`, `Squirrel.osm`, config files). Display and audio settings come from the
   game's own config.

Without `--install` the exe is left in `build\win\ss2_native.exe`. The exe writes `ss2_native.log` next to itself; after a
crash it contains a report (last recompiled functions, native calls, stack, loaded modules).

`host/build_win.py` also runs on Linux or macOS (`python3 host/build_win.py /path/to/SS2.exe`), but it builds the native
Windows executable, which only runs on Windows. To play on Linux, use the portable host below.

### Switch Files

Empty text files placed next to the exe change its behaviour:

Empty text files placed next to the exe change its behaviour:

| File | Effect |
| --- | --- |
| `darkrecomp_debug.txt` | verbose diagnostics: per-frame draw statistics, call tracing, heartbeat, and a few backbuffer screenshots (`darkrecomp_*.bmp`) |
| `darkrecomp_windowed.txt` | force windowed mode |
| `darkrecomp_nomsaa.txt` | force multisampling off |
| `darkrecomp_novsync.txt` | present without vsync |
| `darkrecomp_nolgvid.txt` | hide the video decoder (skips cutscenes) |
| `darkrecomp_native_osm.txt` | use the original `allobjs.osm`, `Squirrel.osm`, `lgvid.dll` and `fmsel.dll` instead of the recompiled ones |
| `darkrecomp_apistats.txt` | record which Direct3D / DirectSound / DirectInput calls the game makes (counts, formats, pools) into `darkrecomp_api_usage.txt`; input for the platform layer (`port/`) |
| `darkrecomp_native_ffmpeg.txt` | decode cutscenes with the original `ffmpeg.dll` instead of the built-in decoder (needed only for movies in formats the built-in one doesn't play; see *Known issues*) |
| `darkrecomp_heapcheck.txt` | validate all heaps after every native call (slow; for tracking corruption) |
| `darkrecomp_realquery.txt` | use the real D3D frame-limiter query instead of the shortcut |

## Quick Start: Linux and Other Platforms

The portable host ([`port/`](port/README.md)) runs the same recompiled game without Windows and without an x86 CPU. Shared
Win32, C runtime and DirectX front ends sit on one small interface, [`port/include/plat.h`](port/include/plat.h); a port
writes a backend for that interface and nothing else. Included: POSIX (memory, threads, files), Windows (development and
test target), SDL2 (window, input, audio, OpenGL 2.1 / ES 2.0) and a headless backend.

On Ubuntu, Debian or WSL:

```sh
sudo apt install -y git build-essential cmake ninja-build python3-venv libsdl2-dev
git clone https://github.com/Owloeb/NewDarkRecompiled.git && cd NewDarkRecompiled
python3 -m venv .venv && source .venv/bin/activate && pip install pefile capstone
python3 port/build.py "/path/to/System Shock 2/SS2.exe"          # about 8 minutes the first time
cd "/path/to/System Shock 2" && ~/NewDarkRecompiled/build/port/ss2port --windowed SS2.exe
```

Put options **before** `SS2.exe`; anything after it goes to the game. Under WSL, keep the game on the Linux filesystem
(reading through `/mnt/c` is slow). Full instructions, options, environment variables and the porting guide are in
[`port/README.md`](port/README.md).

**Status:** plays through Rickenbacker and Body of the Many on Linux (WSL) and as a 64-bit Windows build. Mouselook is
verified on Windows only: WSLg cannot capture the pointer and native Linux is untested. **Gaps:** shaders and cube/volume
textures (the game falls back to fixed-function), C++ exceptions, `fmsel.dll`.

## How It Works

`lift.py` reads your own `SS2.exe`, finds every function (about 21,000) and emits one C
function per x86 function. Registers live in a struct, arithmetic flags are C locals the compiler folds away, and x87 is
modelled with `double`. Guest memory is flat: guest address `a` lives at host `M + a`.

- **Windows Host** (`host/win_host.c`): linked at the game's original address (0x400000), identity-mapped. At start-up it
  loads the `SS2.exe` sections, wires the import table to the real Windows API and redirects every original function
  to its recompiled version. Window procedures, DirectX callbacks, the C runtime's static constructors and script
  modules that call into the engine land in recompiled code too. What still runs as an original binary is Windows itself.
- **Portable Host** (`port/`): implements the Windows API, the C runtime and Direct3D 9 / DirectSound / DirectInput
  itself on top of a small platform interface, so nothing native is needed.
- **Script and helper modules.** `allobjs.osm` (object scripts), `Squirrel.osm` (NewDark scripting), `lgvid.dll` (cutscene
  player) and `fmsel.dll` (fan-mission selector) are recompiled the same way. When the engine loads one, the host checks
  it is the exact file that was recompiled and runs the recompiled code, falling back to the original if a mod ships its
  own copy. `Squirrel.osm` is lifted as if loaded at 0x30000000 and relocated there.
- **Cutscenes without `ffmpeg.dll`.** `lgvid.dll` still asks for the 2011 `ffmpeg.dll` API; `video/lavshim.c` answers
  with an AVI reader, FFmpeg's Indeo 5 decoder and a YUV-to-RGB scaler, all plain C (details in `video/README.md`).
- **Self-modifying Code.** The software renderer patches its own instructions; the lifter maps every such write to the
  exact instruction field, so the game's own patching keeps working.
- **Verification.** Every function and every distinct instruction encoding was differential-tested against an x86
  emulator; numbers and method are in [`docs/RECOMPILER.md`](docs/RECOMPILER.md).

## Known Issues

**Both Hosts**
- C++ exceptions inside recompiled code are not supported (a `throw` stops the game). `setjmp`/`longjmp` work (the
  Squirrel compiler uses them for syntax errors) but, unlike MSVC's, don't run C++ destructors of skipped frames, so such
  an error may leak a little memory.
- The engine's SSE paths are disabled (the recompiler doesn't translate SSE); it uses its x87 fallbacks, as on an old CPU.
- The built-in cutscene decoder plays what the game ships: AVI with Indeo 5 video and PCM audio. Mods with other formats
  (MPEG-4, H.264, MP4/MKV, ADPCM) won't play; the log says why (`LAVSHIM ...` lines).
- `SS2.log` shows `Failed to load script module ...` for `baseelev.osm`, `traps.osm` and one with an unreadable name
  (`+x?A.osm`, error 126). The retail game logs the same; they are harmless leftovers in the engine's default script list.

**Windows Host**
- It is a 32-bit Windows executable (64-bit Windows 10/11 run it fine). Tested only with NewDark 2.48 on an NVIDIA GPU.
- For cutscene formats the built-in decoder can't play, `darkrecomp_native_ffmpeg.txt` switches back to the original
  `ffmpeg.dll`. That DLL frees a few invalid pointers while opening a video (tolerated by the original binary), so the
  host skips frees of pointers that aren't valid heap blocks (a few small leaks per session).
- Many bring-up diagnostics remain in `host/win_host.c`; they are inactive unless `darkrecomp_debug.txt` exists.

**Portable Host:** see [Status and Limitations](port/README.md#status-and-limitations).

## Build On It!

### Symbols

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

### Hooks (for mods)

`build_win.py --hooks hooks.txt --extra-src mymod.c` makes recompiled engine functions call your own C code when they
are entered, without re-lifting: `tools/apply_hooks.py` inserts the calls into the generated C in seconds, and only the
touched files recompile. A hooks file has one line per hook, `0x<function address> <void function(CPU *c)>`. This
repository itself stays vanilla; mods live in their own repositories.

### Roadmap

1. Shakedown on more machines (AMD and Intel GPUs, other Windows versions) and with popular mods.
2. More names: globals and structure layouts, and hand-named functions for the main systems (render, input, physics,
   AI, save/load), in `symbols/manual.sym`.
3. ~~A platform layer so the recompiled game can run beyond 32-bit Windows.~~ Done: see the
   [portable host](port/README.md).
4. Next for the portable host: shaders and cube/volume textures, native Linux mouselook testing, further backends.
5. Speed: `port/build.py --cache-regs` (experimental, off by default) and a sampling profiler (`-DPORT_PROF=ON`) are in
   the portable host; see [Build details](port/README.md#build-details). A PS Vita backend was tried on the
   `vita-backend` branch: it plays, level changes included, but the Vita's CPU manages only about 6 to 20 fps, so it is
   shelved. Its findings are in `port/backends/vita/PERF.md` on that branch.
