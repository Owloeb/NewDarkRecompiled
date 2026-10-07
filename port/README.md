# port/: the portable host (a platform layer for the recompiled game)

The recompiled game is plain C that only touches memory (`M + address`) and calls out for everything the operating system
used to do. This directory is the other half: a host that runs it **without Windows and without x86**. It is split so
that a port to a new platform only writes the small bottom layer.

```
generated C (nd/, ao/, sq/, lv/, fm/, mods/)   the game, vanilla, identical on every platform
   | rt_call_import / rt_call_external
core/    address space, heap (TLSF), thunks, PE loading, guest threads, kernel objects, virtual file system
win32/   KERNEL32 / USER32 / GDI32 / ADVAPI32 / WINMM: files, time, memory, windows, messages, input mapping
crt/     MSVCR90: MSVC-exact printf/scanf, stdio, string, math, qsort/bsearch/rand, std::string, exceptions stubs
dx/      Direct3D 9 (state, vertex processing, formats, render targets), DirectSound (software mixer), DirectInput 7,
         DirectDraw (detection), the built-in cutscene decoder (video.c, stands in for ffmpeg.dll)
   | include/plat.h        <- the only interface a port implements
backends/posix/   system part: memory, time, threads, files   (any POSIX system)
backends/win32/   system part for Windows (a development and test target: to play on Windows use the normal Windows build)
backends/sdl2/    window, input, audio, OpenGL 2.1 / OpenGL ES 2.0 renderer
backends/null/    headless (everything accepted, nothing shown); used for tests
```

Everything above `plat.h` is shared and contains no operating-system calls. `plat.h` is documented in the header itself
(coordinate conventions: Direct3D clip space with the half-pixel offset already applied, top-left texture origin,
`0xAARRGGBB` colours, DIK scan codes for keys).

## Linux quick start (Ubuntu / Debian, including WSL)

You need your own copy of the game (NewDark 2.48 `SS2.exe`); the generated C is built on your machine and never distributed.

```
# 1. tools and libraries
sudo apt update
sudo apt install -y git build-essential cmake ninja-build python3-venv libsdl2-dev

# 2. get the code (the platform layer lives on its own branch)
git clone -b platform-layer https://github.com/Owloeb/NewDarkRecompiled.git
cd NewDarkRecompiled

# 3. Python environment for the lifter (use `source`, do not execute the activate script)
python3 -m venv .venv
source .venv/bin/activate
pip install pefile capstone

# 4. lift + build (about 8 minutes the first time; later runs skip unchanged steps)
python3 port/build.py "/path/to/System Shock 2/SS2.exe"
```

The result is `build/port/ss2port`. Run it from the game folder, **options before the exe**
(anything after `SS2.exe` is passed to the game):

```
cd "/path/to/System Shock 2"
~/NewDarkRecompiled/build/port/ss2port --windowed SS2.exe
# or: python3 port/build.py ... --install   (copies ss2port next to SS2.exe), then ./ss2port --windowed
# with no exe argument it uses SS2.exe in the current folder or next to ss2port
```

Tips:
- **WSL:** keep the game folder on the Linux filesystem (e.g. `~/ss2`, copy it from `/mnt/c/...`); reading through `/mnt/c`
  is very slow. WSL needs WSLg (Windows 11, or Windows 10 with a recent WSL) for a window and sound.
- **Mouse:** WSLg cannot capture the pointer, so mouselook is unreliable there; on native Linux (X11/Wayland) SDL2 uses
  relative mouse mode. If motion is too fast or slow, set `SS2PORT_MOUSE_SCALE` (e.g. `0.5`); `SS2PORT_MOUSE_DEBUG=1` logs events.
- **Saves/config** are written next to the game; set `SS2PORT_WRITE_DIR` to keep the game folder read-only.
- **Problems:** run with `--verbose`; for a crash or hang add `--trace` (bounded, dumped on crash or Ctrl-C) and see the
  options below. Headless check without a window or sound: `--backend null` at build time.

## Building

```
python3 port/build.py "/path/to/System Shock 2/SS2.exe"     # lifts the game and its DLLs, then builds (see port/build.py)
# or by hand: cmake -S port -B build/port -DPORT_GENERATED=out/port [-DPORT_BACKEND=sdl2|null]
build/port/ss2port [options] /path/to/SS2.exe   # options go BEFORE the exe, anything after it is passed to the game;
                                                # with no exe it uses SS2.exe in the current folder or next to ss2port
```

Windows (cross-compiled from Linux or WSL with Zig, `python3 -m pip install ziglang`; SDL2 is fetched automatically):
`python3 port/build.py "/path/to/SS2.exe" --target windows --install`, then in the game folder `.\ss2port.exe --windowed SS2.exe`.
This runs the same shared code as every other port on a real Windows mouse, keyboard and debugger; it is not meant
for playing (the normal Windows build calls Windows and DirectX directly).

Options: `PORT_PREEMPT` (default ON: other guest threads may run at loop heads; needed for games that spin-wait),
`PORT_VIDEO` (default ON: built-in Indeo 5 cutscene decoder, LGPL code from `video/`). Needs GCC or Clang.
Command line: `--frames N --windowed --guest-space MB --verbose --trace --list-missing --list-shims`.
Environment: `SS2PORT_FULLSCREEN`, `SS2PORT_GLES` (force OpenGL ES), `SS2PORT_NOVSYNC`, `SS2PORT_NOSOUND`,
`SS2PORT_NOALERT` (log instead of message boxes), `SS2PORT_MOUSE_SCALE` (multiply mouse motion, e.g. `0.5`),
`SS2PORT_MOUSE_WARP=1` (capture the mouse by re-centring the pointer, for systems where pointer capture does not work),
`SS2PORT_MOUSE_DEBUG=1` (log raw mouse motion and the cursor the game reads and sets), `SS2PORT_WRITE_DIR` (where saves and configuration are written; the game
folder stays read-only).

## Porting to a new platform

1. Copy `backends/null/` to `backends/<yours>/` and select it with `-DPORT_BACKEND=<yours>`. It builds and runs headless.
2. System part (`backends/posix/` is the reference): `plat_mem_*` (reserve address space, commit, discard), time, threads
   (`plat_thread_start`, mutex, condition variable) and file/directory operations. If your platform is POSIX-like you can
   reuse it unchanged with `-DPORT_SYSTEM=posix`.
3. Video and input: open a window or display, deliver key, mouse and quit events as DIK scan codes in `plat_video_poll`.
4. Renderer: `plat_tex_*` and `plat_gfx_*`. You receive clip-space vertices and a fully resolved draw state (blend, depth,
   stencil, up to eight texture stages already reduced to what the front end could not do on the CPU). `sdl2/gl_render.c`
   generates GLSL from that state and is a good template for other APIs.
5. Audio: `plat_audio_*` pulls 16-bit stereo from the shared software mixer.
6. Run `python3 port/tests/run_conformance.py --backend <yours>`; it needs no game files.

The only requirements on the platform: a little-endian CPU, a C99 compiler with GCC extensions (the generated code uses
`__builtin_setjmp`), and enough address space for the guest (by default a compact layout, configurable with
`--guest-space`; 32-bit hosts work with a small guest space).

## Verification

* `tools/check_shims.py`: compares every shim's declared stack-pop count with the MinGW headers/.def files; a wrong count
  corrupts the guest stack. `tools/check_com.py` does the same for COM vtables (generated by `tools/gen_ifaces.py`).
* `tests/run_conformance.py`: builds a game-free 32-bit Windows test program, lifts it with `lift.py` (so it goes through the
  same recompiler as the game) and runs it on the chosen backend: 370 checks on `null`, 397 on `sdl2` (which adds pixel-exact
  rendering checks). Also run clean under ASan, UBSan and TSan, and as a 32-bit build with a small guest space.
  `--target windows --wrap wine` builds and runs the Windows exe (Wine, or natively on Windows).

## Status and limitations

* Plays: start-up, menus, cutscenes with sound, in-game sound, saves (tested by loading Rickenbacker and Body of the Many),
  on Linux (WSL, Mesa software OpenGL) and as a 64-bit Windows build. Mouselook verified on the Windows build; under WSLg
  it cannot be judged (no pointer lock), and native Linux has not been tried yet.
* Rendering is fixed-function only: the game asks for shaders, is told there are none, and uses its fixed-function path.
  Cube and volume textures are not drawn; `ProcessVertices` is not implemented.
* Not done: C++ exceptions and structured exception handling, the SSE and lock-prefixed instructions in the lifter,
  `fmsel.dll` (intentionally refused), a size-optimised build for small devices.
* Reviewed by four independent passes before release (core, Win32/CRT, DirectX, backends and tooling); the findings
  are fixed and have regression checks in the conformance suite.
