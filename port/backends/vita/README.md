# PS Vita backend

Runs the recompiled game on a homebrew-enabled PS Vita. **Status: plays; exploration is fair, firefights are slow.** It
boots and plays on real hardware, level changes and saves included, at about 15-21 fps exploring and 6-8 fps in heavy
combat (444 MHz CPU clock). The game's own code on one core is the limit: rendering, file access and sound run on other
cores and are no longer the bottleneck. **[`PERF.md`](PERF.md)** has the measurements, what helped, what did not, and
what is left to try.

Needs: a Vita with HENkaku/Enso, `libshacccg.suprx` extracted (vitaGL compiles shaders at run time), VitaShell, and the
same `SS2.exe` and DLLs the build was generated from.

## 1. Install vitasdk (once, in WSL/Ubuntu)

```
sudo apt install -y make git cmake python3 curl bzip2
echo 'export VITASDK=/usr/local/vitasdk' >> ~/.bashrc
echo 'export PATH=$VITASDK/bin:$PATH' >> ~/.bashrc
source ~/.bashrc
git clone https://github.com/vitasdk/vdpm ~/vdpm
cd ~/vdpm && ./bootstrap-vitasdk.sh && ./install-all.sh
```

`install-all.sh` installs vitaGL and its dependencies. Optional, later: rebuild vitaGL with `HAVE_SHADER_CACHE=1`
so compiled shaders are kept on the memory card (fewer stutters the second time a scene is drawn).

## 2. Build

Lift the game once with `port/build.py` as for Linux (it writes `out/port`), but add `--backend null --low-dll-bases` (the Vita
has a small guest space, so the game's DLLs must be lifted for load addresses below 32 MB; the PC default is unchanged).
The recommended speed options are included:

```
python3 port/build.py ~/ss2/SS2.exe --backend null --low-dll-bases --cache-regs --fastpaths
```

then:

```
cmake -S port -B build/vita -DPORT_GENERATED=out/port -DPORT_BACKEND=vita -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
cmake --build build/vita -j
```

Optimisation of the generated code (no relift, only a recompile): `-DPORT_GAME_OPT=-O2` instead of the Release `-O3`, or
size for cold code and speed for hot code from a profile: run a `-DPORT_PROF=ON` build for a few minutes of play, copy
`ux0:data/ss2/ss2port_prof.txt` back, then `python3 tools/hotfiles.py ss2port_prof.txt out/port -o hot.txt` and configure with
`-DPORT_GAME_HOT=$PWD/hot.txt` (hot files `-O2`, the rest `-Os`). See `PERF.md` for how to compare builds.

Changes to the host (anything under `port/`) only need the `cmake --build` step; `build.py` relifts only when the lifter, the
game files or a lift option changed. The `out/port` DLL lifts are for low addresses: a later PC build relifts the three DLLs
(not `SS2.exe`).

Result: `build/vita/ss2port.vpk`. It contains code built from the game: never share it.

`--cache-regs` lifts with `lift.py --cache-regs` (guest registers and the x87 stack in C locals, indirect calls through a
per-thread target cache; see [docs/RECOMPILER.md](../../../docs/RECOMPILER.md#register-caching---cache-regs)). Switching it on
or off relifts everything. `--fastpaths` adds native versions of the hottest engine functions (no relift). For a profile of
where the time goes, add `-DPORT_PROF=ON` to the `cmake` line (see `PERF.md`).

With `--cache-regs`, indirect calls and imports are assumed to keep EBX/EBP/ESI/EDI (the calling convention), and
functions the lifter proves to restore them (`lift_preserve.py`) are trusted to. For a test session, add `-DPORT_ABI_CHECK=ON` to the `cmake` line: every such call is checked, and a violation is written to
`ss2port.log` once per call site (`ABI VIOLATION: call at <site> ...`) and corrected. No such lines means the assumption
holds; leave the option off for timing runs. Both addresses of a reported line go in a file for `port/build.py --abi-exempt`
(see `docs/FLAGS.md`). `--fastpaths` can be combined with `--cache-regs`.

`--cache-regs` also keeps the x87 stack in C locals (`lift_x87.py`). If the game ever stops with a guest fault "x87 stack
depth after a call", put the address it names in a file and add `--x87-exempt FILE` to the `build.py` line.

## 3. Install

- Install `ss2port.vpk` with VitaShell.
- Copy the whole game folder (with `SS2.exe`, `Data/`, `OSM/`, `lgvid.dll`, ...) to `ux0:data/ss2/`.
- Optional `ux0:data/ss2/ss2port.txt` with host options, e.g. `--guest-space 256 --verbose`. Vita-only:
  `--swap-sticks` (move with the right stick, look with the left), `--look-speed 150` (percent), `--no-watchdog`,
  `--no-readahead`, `--arm-clock N` (CPU MHz; by default the clocks are raised to 444/222/222/166 but never lowered, so an
  overclocking plugin's setting is kept; `0` leaves them alone; the log's `clocks:` line shows the result). Sound archives (`SND.CRF`, `SND2.CRF`) also go through a small cache with per-sound read-ahead (`sndcache.c`; `--no-sound-cache` turns it off), because several playing sounds interleave their reads and look random to the window.
- Rendering runs on a thread of its own on core 1 (`backends/sdl2/gl_thread.c`): the game thread (core 0) records each frame's draw calls into a 4 MB buffer and moves on while the render thread replays them through vitaGL, at most one frame behind. Audio, file and guest worker threads use core 2. `--no-render-thread` in `ss2port.txt` puts rendering back on the game thread.
- Resolution: `game_screen_size 960 544` in `cam.cfg` (the Vita's screen) costs nothing extra: the GPU is mostly idle.
  Settings in `cam_ext.cfg` that affect the GPU (multisampling, anisotropic filtering, mipmaps) make no difference either.
  The ones that cost CPU, as the file itself notes for slow machines, are `enhanced_aidetect_trace`, `enhanced_aifootstep_trace`,
  `enhanced_corona_trace`, `enhanced_precip_trace` and `sun_runtime_obj_shadows`: comment them out (`;`) for a few percent.

Every start rewrites `ux0:data/ss2/ss2port.log`. Every 5 seconds it adds a `profile:` line: frame rate; `render` (how busy
the render thread on core 1 is) and draw calls per frame; `files` (all threads) and `game thread` (the part that blocks the
game); `game waits for renderer`; and `game cpu`, the game thread's own work. Every 15 s a `sound cache:` line says how many
sound reads came from memory, how many went to the card, and whether those were re-reads (a bigger cache would help) or new. If the game neither draws nor touches a file for 90 seconds, a
watchdog crashes it on purpose so the Vita writes a core dump showing where it hung (`--no-watchdog` turns that off).
The log also shows it shows the free memory at start, after the guest block and after
vitaGL, then everything the host logs. Send that file when something goes wrong.

All options, including the Vita ones, are also listed in [`docs/FLAGS.md`](../../../docs/FLAGS.md).

## Memory

The Vita has no lazy commit, so the guest block is real RAM from the start. The game reserves about 166 MB, but only
about 90 MB is ever touched; most of the difference is one 64 MB pool it uses from the bottom up (about 5 MB in use).
So the host reserves `--guest-space` (default 208 MB) of address space but backs only `--guest-backed` (default 128 MB)
with real RAM, and places that pool across the end of the backed part with just its first 12 MB real. A game that
reaches beyond that crashes with a data abort instead of getting memory. The host's own heap is `VITA_HEAP_MB` (40 MB):
about 8 MB of host data, the render thread's 4 MB command ring, and the sound cache, which grows to 24 MB but stops while the
heap has less than 8 MB free. Thread stacks come from the few MB of user RAM left after vitaGL (the render thread takes 1 MB).
vitaGL gets a fixed `VITA_VGL_RAM_MB` (6 MB) pool of RAM and 96 MB of video memory. All of these are compile-time defaults in `plat_vita.c`
(the first two also options in `ss2port.txt`); the log's free-memory lines show what to change.

## Controls

See the table at the top of `plat_vita.c`: left stick moves (W/A/S/D), right stick looks, R fires, L uses, front touch
is the mouse cursor for inventory, PDA and hacking screens. No on-screen keyboard yet, so text entry (such as save names) does not work.
