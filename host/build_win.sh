#!/bin/sh
# build_win.sh <SS2.exe (NewDark 2.48)> [outdir]
# Lifts SS2.exe to C, then cross-compiles the 32-bit Windows host (ss2_native.exe) with Zig's clang.
# Needs: python3 + pefile + capstone, `pip install ziglang`.
set -e
EXE=$1; OUT=${2:-out/nd}; B=build/win
export ZIG_GLOBAL_CACHE_DIR=${ZIG_GLOBAL_CACHE_DIR:-/tmp/zigcache} ZIG_LOCAL_CACHE_DIR=${ZIG_LOCAL_CACHE_DIR:-/tmp/zigcache}
[ -f $OUT/nd_meta.json ] || python3 lift.py --smc "$EXE" nd $OUT
python3 host/gen_hostdata.py $OUT/nd_meta.json "$EXE" $OUT/nd_hostdata.c
mkdir -p $B
CF="-target x86-windows-gnu -O1 -fno-sanitize=undefined -fno-stack-protector -fno-strict-aliasing -fwrapv -w -DRT_IDENTITY -DRT_RING -Iruntime -I$OUT"
python3 -m ziglang cc -target x86-windows-gnu -c host/guest_region.s -o $B/00_guest_region.o
ls $OUT/*.c host/win_host.c | xargs -P${JOBS:-2} -I{} sh -c "python3 -m ziglang cc $CF -c {} -o $B/\$(basename {} .c).o"
python3 -m ziglang cc -target x86-windows-gnu -mwindows -o ${3:-$B/ss2_native.exe} $B/*.o \
    -Wl,--image-base=0x400000 -Wl,--stack,1048576 -luser32 -lkernel32 -lm
python3 host/fix_pe.py ${3:-$B/ss2_native.exe}
echo built ${3:-$B/ss2_native.exe}
