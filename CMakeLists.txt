# FC3PerformancePatch - build script
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)

project(FC3PerformancePatch VERSION 1.1.0 LANGUAGES C)

# The game is a 32-bit x86 program; a 64-bit build of this DLL can never load.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "FC3PerformancePatch must be built for 32-bit x86.\n"
        "  MSVC : cmake -A Win32 ...\n"
        "  MinGW: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-i686.cmake ...")
endif()

option(FC3PP_BUILD_TESTS "Build the test suite" ON)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
# Static C runtime: the DLL must not depend on a redistributable being installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(fc3pp_options INTERFACE)
target_compile_definitions(fc3pp_options INTERFACE
    _WIN32_WINNT=0x0601 WINVER=0x0601 _CRT_SECURE_NO_WARNINGS _CRT_NON_CONFORMING_SWPRINTFS)

if(MSVC)
    target_compile_options(fc3pp_options INTERFACE
        /W4 /WX /utf-8
        /wd4152     # function/data pointer conversion (GetProcAddress results)
        /wd4054 /wd4055
        /Brepro     # no timestamps: bit-for-bit reproducible objects
        # Never let a local source path reach a binary (__FILE__, debug info).
        "/d1trimfile:${CMAKE_SOURCE_DIR}\\"
        "/pathmap:${CMAKE_SOURCE_DIR}=.")
    target_link_options(fc3pp_options INTERFACE
        /Brepro
        /DYNAMICBASE /NXCOMPAT
        "/PDBALTPATH:%_PDB%"   # embed only the PDB file name, not its folder
        /INCREMENTAL:NO)
else()
    target_compile_options(fc3pp_options INTERFACE
        -Wall -Wextra -Werror -Wno-unused-parameter -Wno-cast-function-type
        -fno-strict-aliasing
        "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=."
        "-fdebug-prefix-map=${CMAKE_SOURCE_DIR}=.")
    target_link_options(fc3pp_options INTERFACE
        -static -static-libgcc
        -Wl,--dynamicbase -Wl,--nxcompat
        -Wl,--no-insert-timestamp
        $<$<NOT:$<CONFIG:Debug>>:-s>)   # strip symbols (and with them any path)
endif()

# ---- pure logic (no OS calls): unit-testable ------------------------------
add_library(fc3pp_core STATIC
    src/ini.c src/config.c src/sigscan.c src/pe_file.c src/cpu_topology.c src/patches.c src/fs_guard.c
    src/tex_rule.c)
target_include_directories(fc3pp_core PUBLIC src)
target_link_libraries(fc3pp_core PUBLIC fc3pp_options)

# ---- Win32 layer -----------------------------------------------------------
add_library(fc3pp_win STATIC
    src/pe_image.c src/log.c src/cpu_shim.c src/laa.c src/vtable.c src/display.c src/audio.c
    src/texguard.c)
target_link_libraries(fc3pp_win PUBLIC fc3pp_core user32)

# ---- the proxy DLL -----------------------------------------------------------
if(MSVC)
    set(FC3PP_DEF src/version_msvc.def)
else()
    set(FC3PP_DEF src/version_mingw.def)
endif()
add_library(fc3pp_version SHARED src/dllmain.c src/proxy.c ${FC3PP_DEF})
set_target_properties(fc3pp_version PROPERTIES
    OUTPUT_NAME version
    PREFIX ""
    IMPORT_PREFIX "fc3pp_"       # keep our import library from shadowing the SDK's version.lib
    ARCHIVE_OUTPUT_NAME fc3pp_version_import)
target_link_libraries(fc3pp_version PRIVATE fc3pp_win winmm)
if(NOT MSVC)
    target_link_options(fc3pp_version PRIVATE -Wl,--enable-stdcall-fixup)
endif()

# ---- distributable package ---------------------------------------------------
set(FC3PP_PACKAGE_DIR "${CMAKE_BINARY_DIR}/package/FC3PerformancePatch")
add_custom_target(package_files ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FC3PP_PACKAGE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:fc3pp_version>" "${FC3PP_PACKAGE_DIR}/version.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/config/FC3PerformancePatch.ini"
        "${CMAKE_SOURCE_DIR}/scripts/install.bat"
        "${CMAKE_SOURCE_DIR}/scripts/install.ps1"
        "${CMAKE_SOURCE_DIR}/scripts/uninstall.bat"
        "${CMAKE_SOURCE_DIR}/scripts/uninstall.ps1"
        "${CMAKE_SOURCE_DIR}/scripts/install.sh"
        "${CMAKE_SOURCE_DIR}/scripts/uninstall.sh"
        "${CMAKE_SOURCE_DIR}/README.md"
        "${CMAKE_SOURCE_DIR}/TECHNICAL.md"
        "${CMAKE_SOURCE_DIR}/LICENSE"
        "${FC3PP_PACKAGE_DIR}/"
    DEPENDS fc3pp_version
    COMMENT "Assembling the release package"
    VERBATIM)

if(FC3PP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
