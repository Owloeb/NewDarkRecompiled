#!/bin/sh
# port/tests/sndcache/run.sh: the sound cache test at the old 4 MB size and the current default, with ThreadSanitizer.
set -e
W=${SNDCACHE_TEST_DIR:-build/sndcache_test}; mkdir -p $W
for mb in 4 24; do
  cc -O1 -g -fsanitize=thread -DVITA_SOUND_CACHE_MB=$mb -I port/backends/vita port/tests/sndcache/test.c port/backends/vita/sndcache.c -o $W/t$mb -lpthread
  echo "cache $mb MB:"; $W/t$mb $W/SND.CRF
done
