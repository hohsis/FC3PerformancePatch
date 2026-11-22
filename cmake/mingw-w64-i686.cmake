# CMake toolchain file: cross-compile for 32-bit Windows with MinGW-w64.
#
#   cmake -S . -B build-mingw -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-i686.cmake -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-mingw
#   ctest --test-dir build-mingw --output-on-failure      # runs the Windows test programs under Wine
#
# Debian/Ubuntu: apt install gcc-mingw-w64-i686 cmake      (tests: wine32)
# SPDX-License-Identifier: GPL-3.0-or-later

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)

set(FC3PP_MINGW_PREFIX i686-w64-mingw32 CACHE STRING "MinGW-w64 target triplet")

set(CMAKE_C_COMPILER ${FC3PP_MINGW_PREFIX}-gcc)
set(CMAKE_RC_COMPILER ${FC3PP_MINGW_PREFIX}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${FC3PP_MINGW_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Test programs are Windows executables: run them through Wine when it exists.
find_program(FC3PP_WINE NAMES wine wine32 wine-stable)
if(FC3PP_WINE)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${FC3PP_WINE}")
endif()
