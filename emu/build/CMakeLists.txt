# Cross-platform build for Snap Station emulation unit tests and the
# Mupen64Plus input plugin. Supersedes the Win32-only build_*.bat scripts
# (retained on main during Phase 1; archived when their replacement lands
# in Phase 2). The existing top-level Makefile still works for the tests
# on Linux/macOS; this CMake adds MSYS2/Windows coverage and the plugin.

cmake_minimum_required(VERSION 3.16)
project(snap_station_emu C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W3)
else()
  add_compile_options(-Wall -Wextra)
endif()

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
include_directories(${REPO_ROOT}/src)

# --- Unit tests ------------------------------------------------------------
enable_testing()

add_executable(test_sticker_sheet
  ${REPO_ROOT}/src/sticker_sheet.c
  ${REPO_ROOT}/test/test_sticker_sheet.c)
if(NOT MSVC)
  target_link_libraries(test_sticker_sheet PRIVATE m)
endif()
add_test(NAME sticker_sheet
  COMMAND test_sticker_sheet sticker_sheet_demo.bmp)

add_executable(test_joybus
  ${REPO_ROOT}/src/joybus_snapstation.c
  ${REPO_ROOT}/test/test_joybus.c)
add_test(NAME joybus COMMAND test_joybus)

add_executable(test_smart_card
  ${REPO_ROOT}/src/smart_card.c
  ${REPO_ROOT}/test/test_smart_card.c)
add_test(NAME smart_card COMMAND test_smart_card)

# --- Mupen64Plus input plugin (optional) -----------------------------------
# Enable with -DWITH_M64P_PLUGIN=ON once mupen64plus-core headers are on
# the include path (e.g. via MSYS2 package `mingw-w64-x86_64-mupen64plus`
# or a sibling checkout of mupen64plus-core).
option(WITH_M64P_PLUGIN "Build mupen64plus-input-snapstation shared library" OFF)
if(WITH_M64P_PLUGIN)
  add_library(mupen64plus-input-snapstation SHARED
    ${REPO_ROOT}/src/m64p_input_plugin.c
    ${REPO_ROOT}/src/joybus_snapstation.c
    ${REPO_ROOT}/src/smart_card.c
    ${REPO_ROOT}/src/sticker_sheet.c)
  if(NOT MSVC)
    target_link_libraries(mupen64plus-input-snapstation PRIVATE m)
  endif()
endif()

# --- Print bridge (Phase 2 replacement for snap_station_win32.c) -----------
# Populated in Phase 2. Scaffold only.
set(PRINT_BRIDGE_SRC "")
if(WIN32)
  set(PRINT_BRIDGE_SRC ${REPO_ROOT}/src/print_bridge_win32.c)
elseif(APPLE OR UNIX)
  set(PRINT_BRIDGE_SRC ${REPO_ROOT}/src/print_bridge_cups.c)
endif()
if(PRINT_BRIDGE_SRC AND EXISTS ${PRINT_BRIDGE_SRC})
  add_library(print_bridge STATIC ${PRINT_BRIDGE_SRC})
endif()
