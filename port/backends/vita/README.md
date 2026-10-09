# PS Vita backend (experimental)

Runs the recompiled game on a homebrew-enabled PS Vita. Status: builds against the Vita SDK headers and passes the
conformance suite as 32-bit ARM (under qemu, with the Vita system layer); **not yet run on a Vita.**

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

Lift the game once with `port/build.py` as for Linux (it writes `out/port`), then:

```
cmake -S port -B build/vita -DPORT_GENERATED=out/port -DPORT_BACKEND=vita -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
cmake --build build/vita
```

Result: `build/vita/ss2port.vpk`. It contains code built from the game: never share it.

Experimental: `python3 port/build.py <SS2.exe> --backend null --cache-regs` lifts with `lift.py --cache-regs`, which keeps
the guest registers in C locals inside each recompiled function (see `runtime/rt_fast.h`). It passes the conformance suite
(also on ARM) but is not yet known to be faster on the Vita; compare the profile lines in `ss2port.log` with and without it.
Switching it on or off relifts everything.

## 3. Install

- Install `ss2port.vpk` with VitaShell.
- Copy the whole game folder (with `SS2.exe`, `Data/`, `OSM/`, `lgvid.dll`, ...) to `ux0:data/ss2/`.
- Optional `ux0:data/ss2/ss2port.txt` with host options, e.g. `--guest-space 256 --verbose`. Vita-only:
  `--swap-sticks` (move with the right stick, look with the left), `--look-speed 150` (percent).
- Set the game's resolution low (640x480) in its config to begin with.

Every start rewrites `ux0:data/ss2/ss2port.log`. Every 5 seconds it adds a `profile:` line (frame rate, and the share of time
in the renderer, the file layer and the game's own code). If the game neither draws nor touches a file for 90 seconds, a
watchdog crashes it on purpose so the Vita writes a core dump showing where it hung (`--no-watchdog` turns that off).
The log also shows it shows the free memory at start, after the guest block and after
vitaGL, then everything the host logs. Send that file when something goes wrong.

## Memory

The Vita has no lazy commit, so the guest block is real RAM from the start. The game reserves about 166 MB, but only
about 90 MB is ever touched; most of the difference is one 64 MB pool it uses from the bottom up (about 5 MB in use).
So the host reserves `--guest-space` (default 208 MB) of address space but backs only `--guest-backed` (default 128 MB)
with real RAM, and places that pool across the end of the backed part with just its first 12 MB real. A game that
reaches beyond that crashes with a data abort instead of getting memory. The host's own heap is `VITA_HEAP_MB` (40 MB),
vitaGL gets a fixed `VITA_VGL_RAM_MB` (6 MB) pool of RAM and 96 MB of video memory. All of these are compile-time defaults in `plat_vita.c`
(the first two also options in `ss2port.txt`); the log's free-memory lines show what to change.

## Controls

See the table at the top of `plat_vita.c`: left stick moves (W/A/S/D), right stick looks, R fires, L uses, front touch
is the mouse cursor for inventory, PDA and hacking screens. No on-screen keyboard yet, so text entry (such as save names) does not work.
