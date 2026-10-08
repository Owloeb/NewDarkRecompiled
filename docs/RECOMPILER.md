# The recompiler: technical notes

Details of `lift.py` and how it was verified. For using the project, see the [top-level README](../README.md).

Contents: [Layout](#layout) · [Recompiler usage](#recompiler-usage) · [Translation model](#translation-model) · [Verification status](#verification-status) · [Bugs found by the verification](#bugs-found-by-the-verification-all-fixed) · [Known gaps](#known-gaps)


## Layout

```
lift.py               PE -> C: function discovery, CFG, C emission, jump tables, import/indirect dispatch,
                      self-modifying-code analysis (--smc), interprocedural flags analysis
runtime/rt.h          guest CPU state, guest memory access, x87 helpers, call/return macros
harness/hx.c          test host: runs one recompiled function on a given state (faults -> outcomes)
harness/difftest.py   function-level differential test vs Unicorn running the original x86
harness/insnfuzz.py   per-instruction differential fuzzer (every distinct encoding in both binaries)
harness/smc_test.py   targeted test for self-modifying renderer code (live patch values + renderer globals)
harness/tracecmp.py   lockstep trace: first instruction where C and Unicorn diverge (needs a trace build)
build.sh              build generated C + harness into a shared library
build_trace.sh        same, with per-instruction state tracing (RT_TRACE)
```

## Recompiler usage

Run from the repository root.

```sh
python3 lift.py --smc SS2.exe ss out/ss2                  # ~2 min: 16,395 functions -> 111 C files
./build.sh out/ss2 build/ss build/libss.so ss             # ~3 min
python3 lift.py --smc allobjs.osm ao out/allobjs          # ~10 s: 3,716 functions -> 26 C files
./build.sh out/allobjs build/ao build/libao.so ao

python3 harness/difftest.py SS2.exe out/ss2/ss_meta.json build/libss.so --trials 4 --report ss.json
SMC_SMALL=1 SMC_PREPATCH=1 python3 harness/smc_test.py SS2.exe out/ss2/ss_meta.json build/libss.so out/ss2
python3 harness/insnfuzz.py gen out/fz allobjs.osm SS2.exe && ./build.sh out/fz build/fz build/libfz.so fz
python3 harness/insnfuzz.py run out/fz build/libfz.so --trials 24

./build_trace.sh out/ss2 build/sst build/libsst.so ss
python3 harness/tracecmp.py SS2.exe out/ss2/ss_meta.json build/libsst.so 0x<function> <trial>
```

Needs python3, pefile, capstone, unicorn and gcc/clang. Set `UC_IGNORE_REG_BREAK=1` to silence a Unicorn
deprecation warning.

## Translation model

- Guest memory is flat: guest address `a` lives at host `M + a`. Registers live in `CPU`; arithmetic flags are C
  locals per function, so dead ones fold away. x87 is modelled with `double`, matching MSVC's default 53-bit
  precision.
- Each guest function becomes `void <prefix>_<va>(CPU *c)`. `call` pushes the real return address and calls the C
  function; `ret` pops and returns. Tail jumps become tail calls.
- Jump tables (including negative-index and gapped MSVC `memcpy` tables) become C `switch`. Other indirect
  calls go through a sorted address table, falling back to `rt_call_external`.
- Imports call `rt_call_import(c, idx)`. IAT slots hold magic addresses, so `mov reg, [IAT]; call reg` resolves too.
- **Flags across calls.** A whole-program fixpoint finds functions that read flags on entry or return results in
  flags (the CRT float helpers return NaN/Inf in ZF). Spills and reloads are emitted only at those boundaries.
  Indirect calls are treated as flag boundaries.
- **Self-modifying code.** Every absolute write into code is mapped to the exact instruction field it patches.
  SS2.exe has 237 such fields: 4-byte displacements (texture addresses), 4-byte immediates (masks), 1-byte
  immediates (shift counts) and one patched `jg` displacement. A patched instruction reads that field from guest
  memory at runtime, so the game's own patch writes need no translation. The patched branch becomes a `switch`
  over every value the code stores there. Ambiguous overlapping decodes are resolved by linear decode from the
  nearest real function start.
- `fs:` addressing uses `c->fs_base` (TIB; `fs:[0]` = SEH chain).
- Build modes: `RT_SHADOW` verifies return addresses with a shadow stack, `RT_BUDGET` bounds execution,
  `RT_TRACE` records state before every instruction.

## Verification status

**SS2.exe**, function level (16,395 functions x 4 random states; registers, x87 stack and all writable memory,
code included, compared on completion and at the faulting instruction):
- 7,421 functions match on completed runs and 8,845 match exactly at the fault point.
- 51 contain only junk decodes, 39 only reach identical faults, and 7 always exhaust the step budget.
- All 8 remaining failures are explained:
  - 80-bit vs double exponent range (3)
  - NaN sign (1)
  - dead faulting loads removed by GCC (3; verified identical in the trace build)
  - a junk decode (1)
- 10 functions differ only in state at the faulting instruction; lockstep traces agree up to the fault in every case.
- 14 functions write through random pointers into code. Sampled cases are generic copy/clear loops fed pointers
  from data, not self-modification.

**Self-modifying renderer** (20 functions that write to or contain patched code; patch fields and renderer globals
pre-loaded with live values, 48 runs each):
- 459 passing runs executed patched instructions with live values, covering 18 of the 20 functions.
- There are no plain mismatches. All 48 runs of the hardest case agree instruction for instruction in lockstep traces.

**allobjs.osm** (3,716 functions x 8): 1,995 match on completion, 1,711 at the fault point, 5 contain only junk
decodes. The 2 failures are a NaN payload and an `fxam` on an empty x87 register.

**Instruction level** (12,892 stubs / 6,414 shapes from both binaries, 24 states each):
- 308,039 of 308,049 comparable trials pass.
- The 3 failing shapes are a Unicorn bug (`fyl2x` with input <= 0) and subnormal `fmul`.

**Negative control:** a planted `sar`-as-`shr` bug is caught in 13 of 13 `sar` shapes.

## Bugs found by the verification (all fixed)

- Flags returned across `ret`/`call` by CRT assembly. Fixed by the interprocedural flags analysis.
- MSVC `memcpy` jump tables with negative indices and an unused first slot.
- x87 `fsin`/`fcos`/`fptan`/`fsincos` with |x| >= 2^63 must leave the operand unchanged and set C2.
- Pointer scanning of code sections in relocation-less images created thousands of fake function entries,
  including at every patch target.
- Unicorn harness: leftover emulator state across faulting runs, 16-bit stack segment after GDT setup.

## Known gaps

- x87 tag word (empty registers) isn't modelled; `fxam` on an empty slot differs (CRT math-error path only).
- AF flag isn't modelled (no BCD instructions in reached code; `pushfd` reports AF=0).
- x87 precision control is ignored (always 53-bit). Direct3D sets 24-bit on device creation, so renderer math
  will be slightly *more* precise than on Windows. NaN payloads, subnormals and the 80-bit exponent range differ.
- At a fault, multi-step instructions aren't atomic, and GCC may drop or reorder faulting dead loads.
  This only matters for code that is crashing anyway.
- Return addresses don't steer control flow, so setjmp/longjmp and C++ exception unwinding need explicit runtime
  support.
- The mapper variant at 0x52e100-0x52e650 has no references in the exe and is treated as dead. If it's ever
  called, the dispatch fails loudly.
- No optimisation pass yet: flag locals, registers resident in the struct, memcpy-based memory access.
