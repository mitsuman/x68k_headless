cmake_minimum_required(VERSION 3.20)
project(x68k_headless VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(PX68K_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/px68k-libretro)

set(PX68K_C_SOURCES
  ${PX68K_DIR}/libretro.c
  ${PX68K_DIR}/m68000/m68000.c
  ${PX68K_DIR}/m68000/c68k/c68k.c
  ${PX68K_DIR}/m68000/c68k/c68kexec.c
  ${PX68K_DIR}/x68k/adpcm.c
  ${PX68K_DIR}/x68k/bg.c
  ${PX68K_DIR}/x68k/crtc.c
  ${PX68K_DIR}/x68k/dmac.c
  ${PX68K_DIR}/x68k/fdc.c
  ${PX68K_DIR}/x68k/fdd.c
  ${PX68K_DIR}/x68k/disk_d88.c
  ${PX68K_DIR}/x68k/disk_dim.c
  ${PX68K_DIR}/x68k/disk_xdf.c
  ${PX68K_DIR}/x68k/gvram.c
  ${PX68K_DIR}/x68k/ioc.c
  ${PX68K_DIR}/x68k/irqh.c
  ${PX68K_DIR}/x68k/mem_wrap.c
  ${PX68K_DIR}/x68k/mercury.c
  ${PX68K_DIR}/x68k/mfp.c
  ${PX68K_DIR}/x68k/palette.c
  ${PX68K_DIR}/x68k/midi.c
  ${PX68K_DIR}/x68k/pia.c
  ${PX68K_DIR}/x68k/rtc.c
  ${PX68K_DIR}/x68k/sasi.c
  ${PX68K_DIR}/x68k/scc.c
  ${PX68K_DIR}/x68k/scsi.c
  ${PX68K_DIR}/x68k/sram.c
  ${PX68K_DIR}/x68k/sysport.c
  ${PX68K_DIR}/x68k/tvram.c
  ${PX68K_DIR}/libretro/joystick.c
  ${PX68K_DIR}/libretro/keyboard.c
  ${PX68K_DIR}/libretro/mouse.c
  ${PX68K_DIR}/libretro/prop.c
  ${PX68K_DIR}/libretro/timer.c
  ${PX68K_DIR}/libretro/dswin.c
  ${PX68K_DIR}/libretro/windraw.c
  ${PX68K_DIR}/libretro/winui.c
  ${PX68K_DIR}/libretro/dosio.c
  ${PX68K_DIR}/libretro/fake.c
  ${PX68K_DIR}/libretro/peace.c
  ${PX68K_DIR}/libretro/state.c
  ${PX68K_DIR}/libretro-common/string/stdstring.c
)

set(PX68K_CXX_SOURCES
  ${PX68K_DIR}/fmgen/fmg_wrap.cpp
  ${PX68K_DIR}/fmgen/fmgen.cpp
  ${PX68K_DIR}/fmgen/fmtimer.cpp
  ${PX68K_DIR}/fmgen/opm.cpp
  ${PX68K_DIR}/fmgen/opna.cpp
  ${PX68K_DIR}/fmgen/psg.cpp
)

add_library(px68k_core STATIC ${PX68K_C_SOURCES} ${PX68K_CXX_SOURCES})

target_include_directories(px68k_core PUBLIC
  ${PX68K_DIR}
  ${PX68K_DIR}/libretro-common/include
  ${PX68K_DIR}/libretro
  ${PX68K_DIR}/x68k
  ${PX68K_DIR}/fmgen
  ${PX68K_DIR}/m68000
)

target_compile_definitions(px68k_core PUBLIC
  HAVE_C68K
  C68K_NO_JUMP_TABLE
  NO_MERCURY
  __LIBRETRO__
  PX68K_VERSION="0.15+"
  GIT_VERSION=" 0ad84d7"
)

target_compile_options(px68k_core PRIVATE
  -O2
  -w
  -fno-strict-aliasing
)

# The vendored core is C89-ish and contains SJIS string literals we do not want
# clang to complain about.
set_source_files_properties(${PX68K_DIR}/libretro.c PROPERTIES COMPILE_OPTIONS "-Wno-invalid-source-encoding")

add_executable(x68k
  src/main.c
  src/app.c
  src/client.c
  src/emu.c
  src/ipc.c
  src/json.c
  src/keys.c
  src/log.c
  src/png.c
  src/recorder.c
  src/server.c
  src/util.c
  src/vgm.c
)
target_include_directories(x68k PRIVATE src)
target_compile_definitions(x68k PRIVATE X68K_VERSION="${PROJECT_VERSION}")
target_link_libraries(x68k PRIVATE px68k_core)
target_compile_options(x68k PRIVATE -O2 -g -Wall -Wextra)
find_package(ZLIB REQUIRED)
target_link_libraries(x68k PRIVATE ZLIB::ZLIB m)

install(TARGETS x68k RUNTIME DESTINATION bin)
