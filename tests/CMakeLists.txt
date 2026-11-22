# FC3PerformancePatch - tests
# SPDX-License-Identifier: GPL-3.0-or-later

find_package(Python3 COMPONENTS Interpreter)

# ---- unit tests ---------------------------------------------------------------
add_executable(fc3pp_unit
    unit/main.c unit/dummy_pe.c
    unit/test_ini.c unit/test_sigscan.c unit/test_pe_file.c unit/test_cpu.c
    unit/test_patches.c unit/test_pe_image.c unit/test_fs_guard.c unit/test_tex_rule.c)
target_link_libraries(fc3pp_unit PRIVATE fc3pp_win)
add_test(NAME unit COMMAND fc3pp_unit)

# ---- integration test against the user's own game binaries ----------------------
# Skipped (exit code 77) unless FC3PP_GAME_BIN names one or more "bin" folders.
add_executable(fc3pp_realbin realbin/test_realbin.c)
target_link_libraries(fc3pp_realbin PRIVATE fc3pp_win)
if(MSVC)
    target_link_options(fc3pp_realbin PRIVATE /ENTRY:wmainCRTStartup)
else()
    target_link_options(fc3pp_realbin PRIVATE -municode)
endif()
add_test(NAME real_game_binaries COMMAND fc3pp_realbin)
set_tests_properties(real_game_binaries PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)

# ---- runtime smoke tests: the proxy inside a 32-bit host -------------------------
# Everything lands in one folder so the host finds its fixtures.
set(FC3PP_RUNTIME_DIR "${CMAKE_BINARY_DIR}/runtime")

function(fc3pp_runtime_target target)
    set_target_properties(${target} PROPERTIES
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${FC3PP_RUNTIME_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${FC3PP_RUNTIME_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${FC3PP_RUNTIME_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${FC3PP_RUNTIME_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${FC3PP_RUNTIME_DIR}")
    target_link_libraries(${target} PRIVATE fc3pp_options)
endfunction()

# Engine fixtures: same source, built under both engine file names. The DX9
# one has no enumeration loop, which exercises the "signature not found" path.
# The DX11 one imports CreateDXGIFactory1 from "dxgi.dll" through the system's
# import library; at run time that binds to the fake dxgi.dll built below,
# because it sits next to the host.
add_library(fx_engine_d3d11 SHARED runtime/fixture_engine.c)
set_target_properties(fx_engine_d3d11 PROPERTIES OUTPUT_NAME FC3_d3d11)
target_link_libraries(fx_engine_d3d11 PRIVATE version dxgi)
fc3pp_runtime_target(fx_engine_d3d11)

add_library(fx_engine_dx9 SHARED runtime/fixture_engine.c)
set_target_properties(fx_engine_dx9 PROPERTIES OUTPUT_NAME FC3)
target_compile_definitions(fx_engine_dx9 PRIVATE FX_NO_LOOP FX_NO_DXGI FX_NO_TEXTURES)
target_link_libraries(fx_engine_dx9 PRIVATE version)
fc3pp_runtime_target(fx_engine_dx9)

# Fake dxgi.dll: a factory and swap chains with the real virtual table layout.
# Nothing links against it (its import library gets a name that cannot shadow
# the system's dxgi import library).
add_library(fx_dxgi SHARED runtime/fixture_dxgi.c)
set_target_properties(fx_dxgi PROPERTIES
    OUTPUT_NAME dxgi
    IMPORT_PREFIX "fc3pp_"
    ARCHIVE_OUTPUT_NAME fx_dxgi_import)
if(NOT MSVC)
    # Export CreateDXGIFactory1 without the stdcall "@8" suffix, as dxgi.dll does.
    target_link_options(fx_dxgi PRIVATE -Wl,--kill-at)
endif()
fc3pp_runtime_target(fx_dxgi)
add_dependencies(fx_engine_d3d11 fx_dxgi)

add_library(fx_sysdet SHARED runtime/fixture_sysdet.c)
set_target_properties(fx_sysdet PROPERTIES OUTPUT_NAME systemdetection)
fc3pp_runtime_target(fx_sysdet)

add_executable(fc3pp_harness runtime/harness.c)
target_link_libraries(fc3pp_harness PRIVATE fx_engine_d3d11 version)
fc3pp_runtime_target(fc3pp_harness)

add_executable(fc3pp_harness_dx9 runtime/harness.c)
target_link_libraries(fc3pp_harness_dx9 PRIVATE fx_engine_dx9 version)
fc3pp_runtime_target(fc3pp_harness_dx9)

add_executable(fc3pp_passive runtime/passive.c)
target_link_libraries(fc3pp_passive PRIVATE version)
fc3pp_runtime_target(fc3pp_passive)

# The hosts must start out NOT large-address-aware, like the game executables.
foreach(host fc3pp_harness fc3pp_harness_dx9 fc3pp_passive)
    if(MSVC)
        target_link_options(${host} PRIVATE /LARGEADDRESSAWARE:NO)
    else()
        target_link_options(${host} PRIVATE -Wl,--disable-large-address-aware)
    endif()
endforeach()

if(Python3_Interpreter_FOUND)
    # ---- static checks on the built DLL: export table, imports, no leaked paths ----
    add_test(NAME export_table
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/check_binary.py" "$<TARGET_FILE:fc3pp_version>")

    # ---- OPSEC / privacy audit ---------------------------------------------------------
    add_test(NAME opsec_selftest
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/opsec_audit.py" --self-test)
    add_test(NAME opsec_sources
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/opsec_audit.py" --tree
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(NAME opsec_package
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/opsec_audit.py"
                --binaries "${CMAKE_BINARY_DIR}/package" --paths "${CMAKE_BINARY_DIR}/package"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    add_test(NAME runtime
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/runtime/run_runtime.py"
                --bin-dir "${FC3PP_RUNTIME_DIR}"
                --dll "$<TARGET_FILE:fc3pp_version>"
                --work-dir "${CMAKE_BINARY_DIR}/runtime-work"
                "--emulator=${CMAKE_CROSSCOMPILING_EMULATOR}")
    set_tests_properties(runtime PROPERTIES TIMEOUT 900)
endif()

# ---- installer / uninstaller round trips (in a throw-away fake Steam library) --------
if(CMAKE_HOST_WIN32)
    # Stands in for a running game in the "is the game running?" checks.
    add_executable(fc3pp_sleeper install/sleeper.c)
    target_link_libraries(fc3pp_sleeper PRIVATE fc3pp_options)
    add_test(NAME installer_windows
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/install/test_install.ps1"
                -PatchDll "$<TARGET_FILE:fc3pp_version>"
                -SleeperExe "$<TARGET_FILE:fc3pp_sleeper>")
    set_tests_properties(installer_windows PROPERTIES TIMEOUT 900)
else()
    find_program(FC3PP_BASH bash)
    if(FC3PP_BASH)
        add_test(NAME installer_linux
            COMMAND ${FC3PP_BASH} "${CMAKE_CURRENT_SOURCE_DIR}/install/test_install.sh" "$<TARGET_FILE:fc3pp_version>")
        set_tests_properties(installer_linux PROPERTIES TIMEOUT 900)
    endif()
endif()
