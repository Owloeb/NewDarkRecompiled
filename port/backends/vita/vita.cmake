# vita.cmake: the PS Vita build (vitasdk + vitaGL), included by port/CMakeLists.txt when PORT_BACKEND=vita.
#
#   cmake -S port -B build/vita -DPORT_GENERATED=out/port -DPORT_BACKEND=vita -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
#   cmake --build build/vita
#
# The result is build/vita/ss2port.vpk. It contains only the host and the recompiled code built on your machine; the
# game files go to ux0:data/ss2/ on the Vita separately (see plat_vita.c). Never share the VPK: it is built from the
# game's code.
if(NOT DEFINED ENV{VITASDK})
  message(FATAL_ERROR "PORT_BACKEND=vita needs vitasdk: set VITASDK and pass -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake")
endif()
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "System Shock 2" CACHE STRING "name on the LiveArea bubble")
set(VITA_TITLEID "SSHK00002" CACHE STRING "title ID: 4 capital letters and 5 digits")
set(VITA_VERSION "01.00" CACHE STRING "app version, NN.NN")

set(SYS_SRC "")                                                                 # backends/vita brings its own sys/fs part
list(APPEND BACKEND_SRC ${CMAKE_CURRENT_SOURCE_DIR}/backends/sdl2/gl_render.c)    # the shared OpenGL ES 2.0 renderer

# vita-elf-create appends about 3.5 KB of SCE module data after the code segment, and fails ("Cannot allocate N bytes for SCE
# data at end of segment 0; segment 1 overlaps") when the data segment starts closer than that. The default linker script
# starts it at the next 64 KB boundary, so any build whose code happens to end within 3.5 KB of a boundary fails, and a
# fixed -DVITA_CODE_PAD only moves which code sizes fail. Instead, use the toolchain's own default script with one line
# added: always leave 4 KB before the data segment. (Generated from the linker in use, so it follows vitasdk updates.)
execute_process(COMMAND ${CMAKE_LINKER} --verbose OUTPUT_VARIABLE VITA_LD_VERBOSE RESULT_VARIABLE VITA_LD_RES)
string(FIND "${VITA_LD_VERBOSE}" "==================================================" VITA_LD_A)
string(FIND "${VITA_LD_VERBOSE}" "==================================================" VITA_LD_B REVERSE)
string(FIND "${VITA_LD_VERBOSE}" "DATA_SEGMENT_ALIGN" VITA_LD_DSA)
if(VITA_LD_RES EQUAL 0 AND VITA_LD_A GREATER -1 AND VITA_LD_B GREATER VITA_LD_A AND VITA_LD_DSA GREATER -1)
  math(EXPR VITA_LD_A "${VITA_LD_A} + 50")
  math(EXPR VITA_LD_LEN "${VITA_LD_B} - ${VITA_LD_A}")
  string(SUBSTRING "${VITA_LD_VERBOSE}" ${VITA_LD_A} ${VITA_LD_LEN} VITA_LD_SCRIPT)
  string(REPLACE ". = DATA_SEGMENT_ALIGN" ". = . + 0x1000; /* room for vita-elf-create's SCE data (ss2port vita.cmake) */\n  . = DATA_SEGMENT_ALIGN"
         VITA_LD_SCRIPT "${VITA_LD_SCRIPT}")
  file(WRITE ${CMAKE_BINARY_DIR}/vita_sce_gap.ld "${VITA_LD_SCRIPT}")
  set(VITA_LINK_GAP -Wl,-T,${CMAKE_BINARY_DIR}/vita_sce_gap.ld)
else()
  message(WARNING "could not read the default linker script from ${CMAKE_LINKER}; builds may hit vita-elf-create's 'Cannot allocate N bytes for SCE data' (then pass -DVITA_CODE_PAD=<bytes>)")
endif()

function(vita_setup_target t)
  target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/backends/sdl2)
  target_link_options(${t} PRIVATE ${VITA_LINK_GAP})
  target_link_libraries(${t} PRIVATE
    vitaGL vitashark SceShaccCgExt taihen_stub SceShaccCg_stub mathneon
    SceKernelDmacMgr_stub SceGxm_stub SceDisplay_stub SceCommonDialog_stub SceAppMgr_stub SceAppUtil_stub SceSysmodule_stub
    SceCtrl_stub SceTouch_stub SceAudio_stub ScePower_stub SceRtc_stub SceLibKernel_stub
    pthread stdc++ m c)
  vita_create_self(${t}.self ${t} UNSAFE)            # extended permissions, as most vitaGL homebrew is built
  vita_create_vpk(${t}.vpk ${VITA_TITLEID} ${t}.self VERSION ${VITA_VERSION} NAME ${VITA_APP_NAME})
endfunction()
