#!/bin/sh
# build.sh <out directory from the lifter (contains nd/, ao/, sq/, lv/, fm/, mods/)> <output binary>
# Builds the portable host. The generated C is compiled WITHOUT RT_IDENTITY: guest addresses are offsets into one 4 GB block.
#   OPT=-O1 (default)  CC=gcc  JOBS=4  OBJDIR=<where to keep object files, to resume a build>
set -e
HERE=$(cd "$(dirname "$0")" && pwd); OUTDIR=$1; BIN=${2:-ss2port}; OPT=${OPT:--O1}; CC=${CC:-gcc}
OBJ=${OBJDIR:-$OUTDIR/obj}; mkdir -p "$OBJ"
FLAGS="$OPT -w -fwrapv -fno-strict-aliasing -I$HERE/../runtime"
for d in nd ao sq lv fm mods; do
  [ -d "$OUTDIR/$d" ] || continue
  for f in "$OUTDIR/$d"/*.c; do
    o="$OBJ/$(basename "$f" .c).o"
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ]; then
      echo "$CC $f"; $CC $FLAGS -I"$OUTDIR/$d" -c "$f" -o "$o" &
      while [ "$(jobs -p | wc -l)" -ge "${JOBS:-4}" ]; do sleep 0.2; done
    fi
  done
done
wait
$CC -O1 -w -fwrapv -fno-strict-aliasing -I"$HERE/../runtime" -I"$OUTDIR/nd" "$HERE"/host.c "$HERE"/win32.c "$HERE"/crt.c "$HERE"/com.c "$HERE"/mmio.c "$HERE"/modules.c "$HERE"/win32b.c "$OBJ"/*.o -lm -o "$BIN"
echo "built $BIN"
