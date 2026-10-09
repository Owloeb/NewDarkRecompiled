# Making mods (hooks)

This repository stays vanilla: it reproduces the original game and adds nothing to it. Mods live in their own repositories
and attach through hooks.

`host/build_win.py --hooks hooks.txt --extra-src mymod.c` makes recompiled engine functions call your own C code when they
are entered, without re-lifting: `tools/apply_hooks.py` inserts the calls into the generated C in seconds, and only the
touched files recompile.

A hooks file has one line per hook:

    0x<function address> <void function(CPU *c)>

Your C file sees `runtime/rt.h`. Use `docs/NAMING.md` to find the function you want, or `tools/where.py <exe> <address>`
to disassemble around an address.

Hooks work with the Windows exe (`host/build_win.py`). The portable host does not take hooks.
