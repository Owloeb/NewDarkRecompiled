# Windows host (ss2_native.exe)

See the top-level README.md for the overview, the quick start and the switch files.

Short version: build with `python host\build_win.py "C:\path\to\SS2.exe" --install` (needs python3, pefile,
capstone and `pip install ziglang`; about 20 minutes), copy the exe into your System Shock 2 folder and run it.
Output: `ss2_native.log` (short unless `darkrecomp_debug.txt` exists). The generated C and the built exe are derived from
your copy of the game and are not for redistribution.
