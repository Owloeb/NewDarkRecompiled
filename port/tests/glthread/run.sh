#!/bin/sh
# port/tests/glthread/run.sh: builds the render-thread stress test (stress.c) with ThreadSanitizer and runs a few seeds.
set -e
D=$(dirname "$0"); W=${GLTHREAD_TEST_DIR:-build/glthread_test}; mkdir -p $W
cc -O1 -g -fsanitize=thread -I port/include -I port/backends/sdl2 $D/stress.c port/backends/sdl2/gl_thread.c -o $W/stress -lpthread
for s in 1 2 3 4; do $W/stress $s 150000; done
echo "glthread: all seeds passed"
