# NewDark 2.48 target (VR track)

Same toolchain as the original-exe recompile (`lift.py`, runtime, harnesses). This file records what we know about
NewDark so far. **Status: lifted, compiled and verified (see Verification below). Not yet booted.**

## Inputs (fresh GOG install)

| File | Notes |
|---|---|
| SS2.exe | ProductVersion 2.48, built 2019-06-15, MSVC 2008 (linker 9.0), PE32 x86, has relocations, ASLR/NX flags |
| allobjs.osm | **byte-identical** to the 1999 module (sha256 096c3a1e…a645): the verified `ao` recompile applies unchanged |
| squirrel.osm | byte-identical to the earlier upload (Squirrel 3.1 bridge, v1.02, 2018) |

## Lift results

```sh
python3 lift.py --smc SS2.exe nd out/nd        # ~1 min, ~2 GB RAM, 23,157 functions -> 156 C files
```

- Self-modifying code: 242 code writes -> 239 patched fields, 1 patched branch, 0 unresolved (same software texture
  mappers as 1999).
- Flags analysis: 102 functions read flags on entry, 19 export flags on return.
- Floating point is x87 (VS2008 default, no /arch:SSE2): already supported and verified.
- SSE2 is confined to ~10 functions:
  - 0x519930, 0x5199c1, 0x519a43, 0x519b2b, 0x519c2c: 16-bit pixel blend/scale (movdqa/movdqu, punpck*, pmulhuw,
    paddw, packuswb, pshufd)
  - 0x6c6700: CRT memset (movntdq/movnti/sfence)
  - 0x6f77fe: CPUID feature detection
  - 0x47d9a0, 0x6f71d0, 0x6f77ae: one-off movups / cvttsd2si / movapd
  - Plan: add ~15 SSE2 instructions to the lifter and fuzz them, or have CPUID report no SSE2 so fallbacks run.
- Other new forms: `fcomip` (sets EFLAGS from an x87 compare) and the `repz ret` idiom. The rest of the unimpl list is
  data decoded as code (segment pushes, port I/O, BCD).

## Verification (NewDark 2.48, 2026-10-02)

Lift: **21,232 functions -> 143 C files** (an earlier discovery rule produced 23,157 including ~1,900 phantoms, see below).

- **Function level** (21,232 x 4 random states vs Unicorn): 11,623 PASS, 9,488 PASS_AT_FAULT, 18 FAULT_ONLY, 68 UNIMPL
  (junk decodes + the SSE2 functions), and 35 non-passing, all classified:
  - 8 FAIL: NaN payload/sign (4), x87 80-bit exponent range (1), Unicorn sign-extends `ret imm16` (1; hardware zero-extends,
    we are right), deliberate CPUID change (2: 0x6f77fe, 0x6f7860).
  - 16 FAULT_STATE_DIFF: lockstep traces agree instruction-for-instruction up to the fault (15); one NaN sign (0x6114e0).
  - 11 UNKNOWN_SMC: generic rep-movs/clear loops writing through random pointers into code (lockstep trace artefact of
    per-iteration `rep` hooks).
- **Instruction fuzzer** (10,826 shapes / 22,956 stubs from legacy exe + allobjs + NewDark, 24 states each): 548,238 trials
  pass. Failing shapes: Unicorn `fyl2x` bug, subnormal `fmul`, `lahf` (AF not modelled, masked), `test [mem], imm` via the
  undocumented F7 /1 alias (Unicorn rejects it), and `fcomip` which was a **real bug in the first implementation, fixed**
  (2,000/2,000 incl. NaN/inf inputs now pass). Unicorn leaves OF/SF unchanged after FCOMI; the Intel SDM clears them, so the
  fuzzer compares ZF/PF/CF with Unicorn and checks OF=SF=0 against the SDM.
- **Self-patching renderer**: 21 functions write code or contain patched instructions (20 in the 1999 exe). 491 passing
  trials executed live-patched instructions across 19 functions. 136 fault-point state differences in 5 functions;
  sampled lockstep traces (15 trials, 3 functions) agree to the fault.
- **CPUID**: `rt_cpuid` reports GenuineIntel, max leaf 1, no MMX/SSE/SSE2, so the CRT and engine take their non-SSE2
  paths (e.g. `memset` requires `__isa_available >= 2`). The ~10 SSE2 functions stay unimplemented and fail loudly
  (`RT_FAULT_UNIMPL`) if ever reached. To be confirmed at runtime.

### Lifter bug found by this run
With relocations present, every `mov [patch_field], reg` in the self-patching mappers made its target (a mid-instruction
address) a function entry. That produced 239 phantom copies of the mapper loop and ~1,900 junk entries. `discover()` now
seeds from relocations located in code only after everything else is traced, and skips those inside reached instructions.
Removed entries were checked: all are mid-instruction or data-table decodes, none looks like a real function.
New instruction forms added: `fcomi/fcomip/fucomi/fucomip`, `cpuid`, `repz ret`.

## C++ exceptions

- MSVCR90 imports: `__CxxFrameHandler3` (referenced by 1,346 functions, almost all destructor cleanup),
  `_CxxThrowException` (**one** throw site), `_except_handler4_common`, `_XcptFilter`.
- v1: treat a throw as fatal. Later: a runtime `__CxxFrameHandler3` that unwinds recompiled frames and resumes at
  recompiled catch continuations.

## External DLLs (loaded at runtime or imported)

d3d9 + d3dx9_43 (effects, texture filtering), ddraw, dsound / openal32 (wrap_oal), dinput, lgvid.dll (video),
fmsel.dll (fan-mission selector), darkdlgs.dll, MSVCR90/MSVCP90 (C/C++ runtime), dbghelp, psapi, version.

## VR plan

1. Compile and verify NewDark (function diff, instruction fuzz on new encodings, SMC suite).
2. Windows host with identity mapping (guest address == host address): pass imports through to the real DLLs, add
   trampolines for callbacks into guest code (WndProc, timers, enumerators, thread starts), minimal SEH, load
   allobjs + squirrel.osm. Boot NewDark on PC with real D3D9.
3. Locate the camera setup, world render, weapon/hand drawing, aiming and HUD functions (call logging, function
   swapping, frame replay).
4. Stereo: render the world twice per frame into offscreen D3D9 targets without advancing the simulation twice, then
   submit to OpenXR. Then head tracking (view decoupled from body), tracked hands/weapon (left-handed option), HUD on a
   floating panel with pointer input.
