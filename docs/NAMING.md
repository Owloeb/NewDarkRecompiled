# Naming functions

The generated C names every function by its original address (`nd_00601430`). Names make the code readable, make crash
reports useful and are the main way to contribute. There are three sources, from least to most effort.

| Source | Effort | Where it lives |
| --- | --- | --- |
| **Automatic** (`tools/annotate.py`) | none; every build does it | `out/nd/auto.sym` on your machine, never committed |
| **Recorded** (you play, the recorder watches) | an hour of playing | evidence in a report; nothing is named by itself |
| **By hand** (somebody reads the code) | per function | `symbols/manual.sym`, committed, overrides the automatic names |

## 1. Automatic names

`tools/annotate.py` reads your own `SS2.exe` (about 20 seconds) and finds:

- **RTTI:** every polymorphic class and its vtable; each virtual method becomes `Class::vfN`.
- **Constructors and destructors:** functions that store a class's vtable into an object.
- **Console commands:** the engine's command table is ordinary data in the exe (name, handler, help text per entry);
  each handler becomes `cmd_<name>`, with the help text as a note.
- **Import thunks**, and the **string literals** each function uses, as notes.

That names about a third of the roughly 21,000 functions. Nothing derived from the game is committed: everyone who builds
from their own exe gets the same names.

To use them:

- `host/build_win.py ... --named-sources` writes `out/nd_named/`: the generated C with each function's name and notes above
  it, plus `functions.txt`, an index.
- `port/build.py ... --symbols` embeds the names so crash reports print `cmd_crouch+0x14` instead of an address. It adds
  about 780 KB to the binary, so it is off by default. The Windows exe always has them.

## 2. Recording while you play

Some functions have no static trace (the key-binding commands `jump`, `crouch` and `leanleft`, for example, get their
names from tables filled in at run time). A recorder build logs, while you play, which engine functions run, how often,
who calls them, which readable strings they are given and which Windows/DirectX calls each place in the engine makes.

**Build** a recorder variant. It is separate from your normal build; use the normal one to play.

    python host\build_win.py "C:\Games\System Shock 2\SS2.exe" --record          # build\win\ss2_native_rec.exe

**Record.** Copy `ss2_native_rec.exe` next to `SS2.exe` and play. Hold **Ctrl+Alt and press F1..F11** to set the activity
tag (a beep confirms; F12 goes back to tag 0). Everything the game does afterwards is counted under that tag. The method
that works best is **one action per tag**, so the numbers separate cleanly:

| Key | Tag | Do only this |
| --- | --- | --- |
| F1 | 1 | Stand perfectly still, don't touch anything (the baseline). |
| F2 | 2 | Mouse look only: left, right, up, down. |
| F3 | 3 | Walk only: forward, back, strafe. No jumping or crouching. |
| F4 | 4 | Jump only, standing in place. |
| F5 | 5 | Crouch and lean left and right, standing in place. |
| F6 | 6 | Switch weapons only. Don't fire. |
| F7 | 7 | Fire one weapon at a wall. Don't reload. |
| F8 | 8 | Reload only (R), and let it auto-reload once. |
| F9 | 9 | Cycle ammo only (B). |
| F10 | 10 | Open and close the inventory with its key. Don't click anything. |
| F11 | 11 | Anything else you want to isolate. Write down what it was. |

The game writes `darkrecomp_record.txt` next to the exe every 30 seconds and at exit, and also
`darkrecomp_api_usage.txt` (the Direct3D, DirectSound and DirectInput call list). Keep a note of what each tag meant.

**Analyse.** `tags.txt` has one line per tag, `4 jumped in place`.

    python tools/suggest_names.py darkrecomp_record.txt out/nd/nd_meta.json --sym out/nd/auto.sym --sym symbols/manual.sym --tags tags.txt --report report.md --emit out/recorded.sym

The report lists the functions that ran only under each tag, the hottest functions, unnamed functions that call into the
OS or DirectX (with the file names they open), and the files and settings the engine touched.

**What the output is, and is not.** It is a work list with evidence, not names. Two cautions:

- Most of the game runs in every state, so few functions are exclusive to one tag. A function that runs far *more often*
  under a tag than elsewhere is usually a better lead than one that runs only there.
- A string seen in a function's registers at entry may be left over from its caller. Treat it as a lead, and confirm in
  the code before naming anything.

## 3. Names by hand

`symbols/manual.sym` holds names that a person verified by reading the code or from a recording, such as `cmd_crouch`.
It overrides the automatic names, and it is the only naming data in the repository (our own findings, keyed by address).

One line per function:

    0x<address> func <name>   # what it does and how you know

Use `-` as the name for a notes-only line. Contributions are welcome; say in the comment how the name was verified.
