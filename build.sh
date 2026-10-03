#!/bin/sh
# build.sh <gen dir> <objdir> <lib>
set -e
GEN=$1; OBJ=$2; LIB=$3; PFX=${4:-ao}
mkdir -p $OBJ
CF="-O1 -fPIC -fno-strict-aliasing -fwrapv -DRT_BUDGET -DRT_SHADOW -Iruntime -Wall -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable"
ls $GEN/*.c | xargs -P2 -I{} sh -c "gcc $CF -I$GEN -c {} -o $OBJ/\$(basename {} .c).o"
gcc $CF -DLOOKUP=${PFX}_lookup -shared -o $LIB harness/hx.c $OBJ/*.o -lm -lpthread
