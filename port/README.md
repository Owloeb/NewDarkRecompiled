# port/: the portable host (platform layer, shared part)

The recompiled game is plain C that only touches memory (`M + address`) and calls out for everything the operating system
used to do. This directory is the other half: a host that runs it **without Windows and without x86** and gives it the
operating system it expects.

```
generated C (out/nd, out/ao, out/sq, out/lv, out/fm)      <- the game, vanilla, unchanged
  |  rt_call_import / rt_call_external
host.c      guest memory (one 4 GB block), heap, thunks, PE loading, setjmp/longjmp, start-up
win32.c     KERNEL32 / USER32 / GDI32 / ADVAPI32 / WINMM timers: files, time, memory, a fake window
crt.c       MSVCR90 (printf/scanf/stdio/string/math/_CI*) and the few std::string members the game imports
mmio.c      WINMM mmio* (RIFF reading for .wav)
modules.c   loader for the recompiled DLLs (allobjs.osm, Squirrel.osm, lgvid.dll, fmsel.dll)
com.c       Direct3D 9, DirectSound, DirectInput, DirectDraw (detection only) as null back ends
```

Nothing in the generated code changes between platforms. A port replaces the *back ends* below the line; everything above
it is shared.

## Status

* Builds and runs on Linux x86-64 (any POSIX system with a C compiler should work; the host only assumes a little-endian
  CPU and 4 GB of address space to reserve, `mmap` with `MAP_NORESERVE`).
* All 364 imports of `SS2.exe` have an implementation (`ss2port --list-missing` lists any that do not).
* `ss2port --selftest` drives the null Direct3D 9 / DirectSound / DirectInput objects through their vtables and checks the
  stack bookkeeping of each call.
* With the game's `SS2.exe` it runs the C runtime start-up, reads the configuration, detects DirectDraw/DirectX, and goes on
  into engine initialisation (see below for how far it gets with real game data).
* Not done yet: C++ exceptions and structured exception handling, threads (`_beginthreadex` reports failure; the game
  continues single-threaded), WINMM timer callbacks (they would fire on another thread), real windowing/input, real
  rendering and sound.

## Build and run

```
sh port/build.sh out/nd ss2port     # after lifting; compiles out/nd/*.c (+ the modules, see build.sh) and links the host
./ss2port --frames 600 /path/to/SS2.exe
```

Options: `--list-missing` (imports without an implementation), `--trace` (every import call, with string arguments),
`--frames N` (stop after N presented frames), `--selftest`.

`SS2.exe` must be the retail 2.48 executable the C was generated from (the host checks its CRC). The game folder is the
folder of the exe; Windows paths (`Data\res`) are resolved case-insensitively.

## Writing a back end

* **Graphics**: `com.c`, class `C_DEV` and friends. The null device keeps real memory behind `LockRect`/`Lock` so the game's
  texture and vertex uploads work; `Present` counts frames. A real back end implements `CreateTexture`, `SetTexture`,
  `SetRenderState`, `SetSamplerState`, `SetStreamSource`, `SetIndices`, `Draw*`, `Clear`, `Present`, ... on its own API
  (the engine uses the Direct3D 9 fixed-function pipeline plus optional effects that can be switched off in the config).
* **Audio**: class `C_DSB` (DirectSound buffers). Today positions are simulated from the wall clock.
* **Input / window**: `PeekMessageA`/`GetMessageA`, `GetCursorPos`, `GetKeyState`/`GetAsyncKeyState` in `win32.c`, and
  the DirectInput device (`C_DID`, `GetDeviceState`).
* **A different CPU or ABI** needs nothing here: the host is C.

A measured list of the Direct3D calls the game really makes comes from the recorder in the Windows host
(`darkrecomp_apistats.txt`, see the main README): it writes `darkrecomp_api_usage.txt` with call counts, formats and pools.
