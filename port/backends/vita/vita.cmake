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

function(vita_setup_target t)
  target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/backends/sdl2)
  target_link_libraries(${t} PRIVATE
    vitaGL vitashark SceShaccCgExt taihen_stub SceShaccCg_stub mathneon
    SceKernelDmacMgr_stub SceGxm_stub SceDisplay_stub SceCommonDialog_stub SceAppMgr_stub SceAppUtil_stub SceSysmodule_stub
    SceCtrl_stub SceTouch_stub SceAudio_stub ScePower_stub SceRtc_stub SceLibKernel_stub
    pthread stdc++ m c)
  vita_create_self(${t}.self ${t} UNSAFE)            # extended permissions, as most vitaGL homebrew is built
  vita_create_vpk(${t}.vpk ${VITA_TITLEID} ${t}.self VERSION ${VITA_VERSION} NAME ${VITA_APP_NAME})
endfunction()
