#!/bin/sh
# port/tests/f2i/run.sh: fist/fistp conversions (runtime/rt.h F2I32/F2I16) against the libm version, natively and, when
# arm-linux-gnueabihf-gcc and qemu-arm are installed, for 32-bit ARM hard float (Cortex-A9, as on the Vita).
# The flags match the game target in port/CMakeLists.txt.
set -e
W=${F2I_TEST_DIR:-build/f2i_test}; mkdir -p $W
FLAGS="-O2 -fwrapv -fno-strict-aliasing -ffp-contract=off -fno-math-errno -I runtime"
cc $FLAGS port/tests/f2i/test.c -o $W/f2i -lm && $W/f2i
cc $FLAGS -DRT_F2I_LIBM port/tests/f2i/test.c -o $W/f2i_libm -lm && $W/f2i_libm
if command -v arm-linux-gnueabihf-gcc >/dev/null && command -v qemu-arm >/dev/null; then
  arm-linux-gnueabihf-gcc $FLAGS -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -static port/tests/f2i/test.c -o $W/f2i_arm -lm
  qemu-arm $W/f2i_arm
else
  echo "(no arm-linux-gnueabihf-gcc / qemu-arm: ARM run skipped)"
fi
