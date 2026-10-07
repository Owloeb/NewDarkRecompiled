# The portable host: builds the recompiled game (lifter output) together with the platform layer.
#
#   cmake -S port -B build/port -DPORT_GENERATED=<lifter output> [-DPORT_BACKEND=sdl2|null]
#   cmake --build build/port
#
# PORT_GENERATED contains nd/ (SS2.exe) and mods/ (mods.c), plus ao/ sq/ lv/ fm/ for the recompiled DLLs when they
# were lifted. A port to a new platform adds a directory under backends/ and selects it with PORT_BACKEND.
cmake_minimum_required(VERSION 3.16)
project(ss2port C)
set(PORT_GENERATED "" CACHE PATH "lifter output directory (contains nd/ and mods/)")
set(PORT_BACKEND "sdl2" CACHE STRING "platform backend: sdl2 (window, input, audio, OpenGL) or null (headless)")
set(PORT_SYSTEM "posix" CACHE STRING "system part of the platform layer (memory, threads, files)")
option(PORT_PREEMPT "let other guest threads run at loop heads (needed for games that spin-wait)" ON)
if(NOT PORT_GENERATED)
  message(FATAL_ERROR "set -DPORT_GENERATED=<lifter output directory>")
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  message(FATAL_ERROR "the generated code needs GCC or Clang (it uses __builtin_setjmp and friends)")
endif()

# ---- the recompiled game (generated, unchanged between platforms)
file(GLOB GEN_SRC ${PORT_GENERATED}/nd/*.c ${PORT_GENERATED}/ao/*.c ${PORT_GENERATED}/sq/*.c ${PORT_GENERATED}/lv/*.c ${PORT_GENERATED}/fm/*.c ${PORT_GENERATED}/mods/*.c)
set(GEN_DEFS "")
if(PORT_PREEMPT)
  list(APPEND GEN_DEFS RT_PREEMPT)
endif()
add_library(game OBJECT ${GEN_SRC})
target_include_directories(game PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../runtime ${PORT_GENERATED}/nd)
target_compile_definitions(game PRIVATE ${GEN_DEFS})
target_compile_options(game PRIVATE -w -fwrapv -fno-strict-aliasing)

# ---- the shared platform layer: Win32, the C runtime and DirectX on top of plat.h
file(GLOB CORE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/core/*.c ${CMAKE_CURRENT_SOURCE_DIR}/win32/*.c ${CMAKE_CURRENT_SOURCE_DIR}/crt/*.c ${CMAKE_CURRENT_SOURCE_DIR}/dx/*.c)
add_library(portcore STATIC ${CORE_SRC})
target_include_directories(portcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/core ${CMAKE_CURRENT_SOURCE_DIR}/../runtime)
target_compile_definitions(portcore PRIVATE ${GEN_DEFS})
target_compile_options(portcore PRIVATE -Wall -Wno-unused-parameter -Wno-misleading-indentation -Wno-format-truncation -fwrapv -fno-strict-aliasing)

# ---- the backend
file(GLOB SYS_SRC ${CMAKE_CURRENT_SOURCE_DIR}/backends/${PORT_SYSTEM}/*.c)
file(GLOB BACKEND_SRC ${CMAKE_CURRENT_SOURCE_DIR}/backends/${PORT_BACKEND}/*.c)
if(NOT BACKEND_SRC)
  message(FATAL_ERROR "no backend '${PORT_BACKEND}' in backends/")
endif()
add_executable(ss2port ${SYS_SRC} ${BACKEND_SRC} $<TARGET_OBJECTS:game>)
target_include_directories(ss2port PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ss2port PRIVATE portcore m)
find_package(Threads REQUIRED)
target_link_libraries(ss2port PRIVATE Threads::Threads)
if(PORT_BACKEND STREQUAL "sdl2")
  find_package(SDL2 REQUIRED)
  target_link_libraries(ss2port PRIVATE SDL2::SDL2)
endif()
