# Cross-compile the portable host for 64-bit Windows with Zig (python3 -m pip install ziglang):
#   cmake -S port -B build/port-win -DCMAKE_TOOLCHAIN_FILE=port/cmake/windows-zig.cmake -DPORT_GENERATED=... -DSDL2_DIR=...
# port/build.py --target windows does all of this, including fetching SDL2's MinGW development package.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER ${CMAKE_CURRENT_LIST_DIR}/zig-cc)
set(CMAKE_AR ${CMAKE_CURRENT_LIST_DIR}/zig-ar CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB ${CMAKE_CURRENT_LIST_DIR}/zig-ranlib CACHE FILEPATH "" FORCE)
set(CMAKE_RC_COMPILER ${CMAKE_CURRENT_LIST_DIR}/zig-rc)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
