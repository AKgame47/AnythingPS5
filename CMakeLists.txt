# PNR - PlayStation Native Runtime (AnythingPS5)
# Research/prototype build. C++20 core; Rust crates are planned but no
# Rust toolchain exists in the current dev sandbox (documented, not faked).
cmake_minimum_required(VERSION 3.16)
project(pnr_runtime VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Werror=return-type)

# MODULE 01 - core runtime (OS-independent)
add_library(pnr_core core/runtime.cpp)
target_include_directories(pnr_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# MODULE 02 - native executable loader (POSIX today; Windows backend planned)
add_library(pnr_loader loader/elf_parser.cpp loader/native_loader.cpp loader/nid.cpp loader/sony_symbols.cpp)

add_library(pnr_prx prx/prx.cpp prx/prx_file.cpp loader/import_resolver.cpp loader/module_loader.cpp loader/guest_runtime.cpp prx/libc_abi.cpp prx/hw_abi.cpp)
add_library(pnr_synth tools/synth_elf.cpp)
target_link_libraries(pnr_synth PUBLIC pnr_loader)
add_executable(pnr_make_ps_elf tools/make_ps_elf.cpp)
target_link_libraries(pnr_make_ps_elf PRIVATE pnr_synth)

add_library(pnr_psabi SHARED psabi/psabi.cpp)
target_link_libraries(pnr_psabi PRIVATE pnr_compat pnr_shader pnr_graphics)
set_target_properties(pnr_psabi PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(pnr_prx PUBLIC pnr_loader pnr_compat)
target_link_libraries(pnr_loader PUBLIC pnr_core)

# MODULE 03 - compatibility services
add_library(pnr_shader shader/rdna.cpp shader/translate.cpp shader/loop_memory.cpp)
target_include_directories(pnr_shader PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(pnr_graphics graphics/vulkan.cpp graphics/render.cpp graphics/spirv.cpp graphics/pm4.cpp graphics/gfx_state.cpp graphics/gpu_va.cpp graphics/gfx_draw.cpp)
target_include_directories(pnr_graphics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pnr_graphics PUBLIC pnr_core dl)
target_link_libraries(pnr_shader PUBLIC pnr_graphics)
set_target_properties(pnr_graphics PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(pnr_shader PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(pnr_compat compat/services.cpp)
# PIC: pnr_compat also links into the pnr_psabi shared library.
set_target_properties(pnr_compat PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(pnr_compat PUBLIC pnr_core)

# MODULE 05 - adaptive performance engine (deterministic rules)
add_library(pnr_ape ape/ape.cpp)
target_link_libraries(pnr_ape PUBLIC pnr_core)

# MODULE 06 - laptop optimization manager
add_library(pnr_laptop laptop/laptop.cpp)
target_link_libraries(pnr_laptop PUBLIC pnr_ape)

# MODULE 08 - game services: virtual fs, audio, input
add_library(pnr_services services/virtual_fs.cpp services/audio.cpp services/input.cpp services/save_data.cpp)
target_link_libraries(pnr_services PUBLIC pnr_core pnr_compat)

# MODULE 07 - host OS abstraction (Linux + Windows backends)
if(WIN32)
  add_library(pnr_platform platform/host_windows_stub.cpp)
else()
  add_library(pnr_platform platform/host_linux.cpp)
endif()
target_link_libraries(pnr_platform PUBLIC pnr_core)

# Installer - .pnrgame package format + controlled importer
add_library(pnr_package package/sha256.cpp package/tar.cpp package/importer.cpp)
target_link_libraries(pnr_package PUBLIC pnr_core pnr_loader)

# MODULE 09 - game library metadata (SQLite)
# The dev sandbox ships libsqlite3.so without dev headers; the repo carries
# a minimal API shim (library/sqlite3_shim.h). With real headers installed,
# add -DPNR_USE_SYSTEM_SQLITE3 and link the normal way. On Windows there is
# no system SQLite, so build the official amalgamation instead:
#   -DPNR_SQLITE3_AMALGAMATION=<dir containing sqlite3.c/sqlite3.h>
if(WIN32)
  set(PNR_SQLITE3_AMALGAMATION "" CACHE PATH "Directory containing sqlite3.c and sqlite3.h")
  if(NOT PNR_SQLITE3_AMALGAMATION)
    message(FATAL_ERROR "Windows builds need -DPNR_SQLITE3_AMALGAMATION=<sqlite amalgamation dir>")
  endif()
  add_library(pnr_sqlite3 STATIC ${PNR_SQLITE3_AMALGAMATION}/sqlite3.c)
  target_include_directories(pnr_sqlite3 PUBLIC ${PNR_SQLITE3_AMALGAMATION})
  set(SQLITE3_LIBRARY pnr_sqlite3)
  set(PNR_SQLITE3_DEFINE PNR_USE_SYSTEM_SQLITE3)
else()
  find_library(SQLITE3_LIBRARY NAMES sqlite3 libsqlite3.so.0)
  if(NOT SQLITE3_LIBRARY)
    message(FATAL_ERROR "libsqlite3 runtime not found; library module cannot build")
  endif()
endif()
add_library(pnr_library library/game_db.cpp)
target_link_libraries(pnr_library PUBLIC pnr_core ${SQLITE3_LIBRARY})
if(PNR_SQLITE3_DEFINE)
  target_compile_definitions(pnr_library PUBLIC ${PNR_SQLITE3_DEFINE})
endif()

find_package(Threads REQUIRED)
target_link_libraries(pnr_compat PUBLIC Threads::Threads)
target_link_libraries(pnr_loader PUBLIC Threads::Threads pnr_platform)
target_link_libraries(pnr_package PUBLIC pnr_library)

# PSN connector (slice 1 offline core; slice 2 live sync over dlopen-ed libcurl)
add_library(pnr_plugin plugin/semver.cpp plugin/manifest.cpp plugin/manager.cpp plugin/runner.cpp plugin/analyze.cpp plugin/signing.cpp plugin/plugin_cli.cpp)
target_include_directories(pnr_plugin PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pnr_plugin PUBLIC pnr_psn pnr_core pnr_sandbox pnr_store)
add_library(pnr_psn psn/json.cpp psn/psn.cpp psn/psn_cli.cpp psn/http.cpp psn/curl_http.cpp psn/sync.cpp)
target_include_directories(pnr_psn PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pnr_psn PUBLIC pnr_library pnr_core ${CMAKE_DL_LIBS})

# Unofficial store core (slice A1: sources, index validation, search; no downloads yet)
add_library(pnr_store store/store.cpp store/store_cli.cpp store/download.cpp crypto/ed25519.cpp scan/scan.cpp research/research.cpp ask/ask.cpp ask/suggest.cpp launcher/launch_guard.cpp)
target_include_directories(pnr_store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pnr_store PUBLIC pnr_psn pnr_package pnr_core pnr_loader pnr_sandbox pnr_launcher)

# CLI launcher (STEP 1-10 workflow driver)
add_library(pnr_sandbox sandbox/sandbox.cpp)
target_include_directories(pnr_sandbox PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
add_library(pnr_launcher launcher/launcher_core.cpp)
target_include_directories(pnr_launcher PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pnr_launcher PUBLIC pnr_library pnr_loader pnr_core pnr_package pnr_prx)
# Desktop UI model (no GUI code; testable without a display)
add_library(pnr_ui_model ui/ui_model.cpp)
target_include_directories(pnr_ui_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pnr_ui_model PUBLIC pnr_launcher pnr_store pnr_sandbox Threads::Threads)

add_executable(pnr-launcher apps/launcher/launcher_main.cpp)
target_link_libraries(pnr-launcher PRIVATE pnr_launcher pnr_psn pnr_store pnr_sandbox pnr_plugin)
add_executable(pnr apps/pnr_cli.cpp)
target_link_libraries(pnr PRIVATE pnr_core pnr_loader pnr_compat pnr_package pnr_library pnr_platform pnr_ape pnr_laptop pnr_services pnr_prx pnr_graphics pnr_shader)

# Dev tooling: build .pnrgame packages
add_executable(pnr_make_package tools/make_package.cpp)
target_link_libraries(pnr_make_package PRIVATE pnr_package pnr_library pnr_core)

# Developer-controlled native test application ("demo game" v0)
add_executable(pnr_test_app examples/test_app.cpp)
add_executable(ps_demo examples/ps_demo.cpp)
target_link_libraries(ps_demo PRIVATE pnr_psabi)
set_target_properties(ps_demo PROPERTIES BUILD_RPATH "$ORIGIN")
add_executable(ps_mesh_demo examples/ps_mesh_demo.cpp)
target_link_libraries(ps_mesh_demo PRIVATE pnr_psabi)
set_target_properties(ps_mesh_demo PROPERTIES BUILD_RPATH "$ORIGIN")
add_executable(ps_lit_demo examples/ps_lit_demo.cpp)
target_link_libraries(ps_lit_demo PRIVATE pnr_psabi)
set_target_properties(ps_lit_demo PROPERTIES BUILD_RPATH "$ORIGIN")
add_executable(ps_tex_demo examples/ps_tex_demo.cpp)
target_link_libraries(ps_tex_demo PRIVATE pnr_psabi)
set_target_properties(ps_tex_demo PROPERTIES BUILD_RPATH "$ORIGIN")
add_executable(ps_littex_demo examples/ps_littex_demo.cpp)
target_link_libraries(ps_littex_demo PRIVATE pnr_psabi)
set_target_properties(ps_littex_demo PROPERTIES BUILD_RPATH "$ORIGIN")
add_executable(ps_cube_demo examples/ps_cube_demo.cpp)
target_link_libraries(ps_cube_demo PRIVATE pnr_psabi)
set_target_properties(ps_cube_demo PROPERTIES BUILD_RPATH "$ORIGIN")
target_link_libraries(pnr_test_app PRIVATE pnr_compat pnr_core)

enable_testing()
add_executable(pnr_tests tests/test_main.cpp tests/loop_tests.cpp tests/loop_memory_tests.cpp tests/loop_copy_tests.cpp tests/render_tests.cpp tests/cube_tests.cpp tests/rdna_vertex_tests.cpp tests/launcher_tests.cpp tests/launcher_mgmt_tests.cpp tests/launcher_install_tests.cpp tests/launcher_care_tests.cpp tests/psn_tests.cpp tests/psn_sync_tests.cpp tests/store_tests.cpp tests/download_tests.cpp tests/signature_tests.cpp tests/sandbox_tests.cpp tests/scan_tests.cpp tests/research_tests.cpp tests/ask_tests.cpp tests/suggest_tests.cpp tests/ui_model_tests.cpp tests/module_tests.cpp tests/kernel_tests.cpp tests/libc_tests.cpp tests/pm4_tests.cpp tests/gfx_state_tests.cpp tests/gpu_va_tests.cpp tests/gfx_draw_tests.cpp tests/hw_tests.cpp tests/plugin_tests.cpp)

# Let the tests launch the real PS-style demo binary.
target_compile_definitions(pnr_tests PRIVATE
  PNR_PS_DEMO_PATH="$<TARGET_FILE:ps_demo>"
  PNR_PS_CUBE_DEMO_PATH="$<TARGET_FILE:ps_cube_demo>"
  PNR_PS_MESH_DEMO_PATH="$<TARGET_FILE:ps_mesh_demo>"
  PNR_PS_LIT_DEMO_PATH="$<TARGET_FILE:ps_lit_demo>"
  PNR_PS_TEX_DEMO_PATH="$<TARGET_FILE:ps_tex_demo>"
  PNR_PS_LITTEX_DEMO_PATH="$<TARGET_FILE:ps_littex_demo>"
  PNR_PSABI_DIR="$<TARGET_FILE_DIR:pnr_psabi>")
target_link_libraries(pnr_tests PRIVATE pnr_synth pnr_ui_model pnr_core pnr_loader pnr_compat pnr_package pnr_library pnr_platform pnr_ape pnr_laptop pnr_services pnr_prx pnr_graphics pnr_shader pnr_launcher pnr_psn pnr_store pnr_sandbox pnr_plugin)

add_test(NAME pnr_unit_tests COMMAND pnr_tests)
add_test(NAME pnr_kernel_run COMMAND sh -c "$<TARGET_FILE:pnr_make_ps_elf> ${CMAKE_BINARY_DIR}/synth_kernel.elf --kernel && $<TARGET_FILE:pnr> module-run ${CMAKE_BINARY_DIR}/synth_kernel.elf | grep -q \"MODULE FINISHED: exit 0\"")
add_test(NAME pnr_libc_run COMMAND sh -c "$<TARGET_FILE:pnr_make_ps_elf> ${CMAKE_BINARY_DIR}/synth_libc.elf --libc && $<TARGET_FILE:pnr> module-run ${CMAKE_BINARY_DIR}/synth_libc.elf | grep -q \"C3 libc ok 42\"")
add_test(NAME pnr_gnm_decode COMMAND sh -c "$<TARGET_FILE:pnr> gnm-synth ${CMAKE_BINARY_DIR}/synth_pm4.bin && $<TARGET_FILE:pnr> gnm-decode ${CMAKE_BINARY_DIR}/synth_pm4.bin | grep -q \"2 draws\"")
add_test(NAME pnr_module_run COMMAND sh -c "$<TARGET_FILE:pnr_make_ps_elf> ${CMAKE_BINARY_DIR}/synth_ok.elf && $<TARGET_FILE:pnr> module-run ${CMAKE_BINARY_DIR}/synth_ok.elf")
set_tests_properties(pnr_module_run PROPERTIES PASS_REGULAR_EXPRESSION "MODULE FINISHED: exit 30")
# The CLI runs a game that renders a rotating 3D cube through pnrGfx* calls.
add_test(NAME pnr_launcher_list COMMAND pnr-launcher list)
set_tests_properties(pnr_launcher_list PROPERTIES
  ENVIRONMENT "PNR_LIBRARY_DB=${CMAKE_CURRENT_BINARY_DIR}/launcher_ctest_empty.db"
  PASS_REGULAR_EXPRESSION "library is empty")
add_test(NAME pnr_launcher_unknown_id COMMAND pnr-launcher show no.such.game)
set_tests_properties(pnr_launcher_unknown_id PROPERTIES
  ENVIRONMENT "PNR_LIBRARY_DB=${CMAKE_CURRENT_BINARY_DIR}/launcher_ctest_empty.db"
  WILL_FAIL TRUE)
add_test(NAME pnr_cube_run COMMAND pnr run $<TARGET_FILE:ps_cube_demo>)
add_test(NAME pnr_mesh_run COMMAND pnr run $<TARGET_FILE:ps_mesh_demo>)
add_test(NAME pnr_lit_run COMMAND pnr run $<TARGET_FILE:ps_lit_demo>)
add_test(NAME pnr_tex_run COMMAND pnr run $<TARGET_FILE:ps_tex_demo>)
add_test(NAME pnr_littex_run COMMAND pnr run $<TARGET_FILE:ps_littex_demo>)
set_tests_properties(pnr_cube_run PROPERTIES
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  PASS_REGULAR_EXPRESSION "RESULT: RUN OK"
  FAIL_REGULAR_EXPRESSION "FAILED;BLOCKED;ps_cube_demo: FAIL;SKIPPED   demo app")
set_tests_properties(pnr_mesh_run PROPERTIES
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  PASS_REGULAR_EXPRESSION "RESULT: RUN OK"
  FAIL_REGULAR_EXPRESSION "FAILED;BLOCKED;ps_mesh_demo: FAIL;SKIPPED   demo app")
set_tests_properties(pnr_littex_run PROPERTIES
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  PASS_REGULAR_EXPRESSION "RESULT: RUN OK"
  FAIL_REGULAR_EXPRESSION "FAILED;BLOCKED;ps_littex_demo: FAIL;SKIPPED   demo app")
set_tests_properties(pnr_tex_run PROPERTIES
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  PASS_REGULAR_EXPRESSION "RESULT: RUN OK"
  FAIL_REGULAR_EXPRESSION "FAILED;BLOCKED;ps_tex_demo: FAIL;SKIPPED   demo app")
set_tests_properties(pnr_lit_run PROPERTIES
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  PASS_REGULAR_EXPRESSION "RESULT: RUN OK"
  FAIL_REGULAR_EXPRESSION "FAILED;BLOCKED;ps_lit_demo: FAIL;SKIPPED   demo app")
# End-to-end: the CLI validates and natively runs the demo app.
add_test(NAME pnr_end_to_end
         COMMAND pnr run $<TARGET_FILE:pnr_test_app>)
set_tests_properties(pnr_end_to_end PROPERTIES
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# Package workflow: build a .pnrgame from the demo app, import it, list it.
add_test(NAME pnr_package_workflow
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:pnr_test_app>
    -DMAKE=$<TARGET_FILE:pnr_make_package>
    -DPNR=$<TARGET_FILE:pnr>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/package_workflow.cmake)
set_tests_properties(pnr_package_workflow PROPERTIES
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# Headless launcher journey (launcher core + pnr-launcher front end).
add_test(NAME pnr_launcher_workflow
  COMMAND ${CMAKE_COMMAND}
    -DPS_DEMO=$<TARGET_FILE:ps_demo>
    -DMAKE=$<TARGET_FILE:pnr_make_package>
    -DPNR=$<TARGET_FILE:pnr>
    -DLAUNCHER=$<TARGET_FILE:pnr-launcher>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/launcher_workflow.cmake)
set_tests_properties(pnr_launcher_workflow PROPERTIES
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME pnr_launcher_install_workflow
  COMMAND ${CMAKE_COMMAND}
    -DPS_DEMO=$<TARGET_FILE:ps_demo>
    -DMAKE=$<TARGET_FILE:pnr_make_package>
    -DLAUNCHER=$<TARGET_FILE:pnr-launcher>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/launcher_install_workflow.cmake)
set_tests_properties(pnr_launcher_install_workflow PROPERTIES
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME pnr_launcher_care_workflow
  COMMAND ${CMAKE_COMMAND}
    -DPS_DEMO=$<TARGET_FILE:ps_demo>
    -DMAKE=$<TARGET_FILE:pnr_make_package>
    -DLAUNCHER=$<TARGET_FILE:pnr-launcher>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/launcher_care_workflow.cmake)
set_tests_properties(pnr_launcher_care_workflow PROPERTIES
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME pnr_store_workflow
  COMMAND ${CMAKE_COMMAND}
    -DPS_DEMO=$<TARGET_FILE:ps_demo>
    -DMAKE=$<TARGET_FILE:pnr_make_package>
    -DLAUNCHER=$<TARGET_FILE:pnr-launcher>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/store_workflow.cmake)
add_test(NAME pnr_docs_claims
  COMMAND ${CMAKE_COMMAND} -DREPO=${CMAKE_CURRENT_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/docs_claims.cmake)
set_tests_properties(pnr_store_workflow PROPERTIES
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME pnr_psn_workflow
  COMMAND ${CMAKE_COMMAND}
    -DLAUNCHER=$<TARGET_FILE:pnr-launcher>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/psn_workflow.cmake)
set_tests_properties(pnr_psn_workflow PROPERTIES
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# PS-style game journey: package a sce-only binary, import, run BY ID.
add_test(NAME pnr_psgame_workflow
  COMMAND ${CMAKE_COMMAND}
    -DPS_DEMO=$<TARGET_FILE:ps_demo>
    -DMAKE=$<TARGET_FILE:pnr_make_package>
    -DPNR=$<TARGET_FILE:pnr>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/psgame_workflow.cmake)
set_tests_properties(pnr_psgame_workflow PROPERTIES
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# Compact shader audit corpus and deterministic mutation diagnostics.
add_executable(pnr_shader_checks tests/shader_fixtures.cpp)
target_link_libraries(pnr_shader_checks PRIVATE pnr_shader)
add_test(NAME pnr_shader_fixtures COMMAND pnr_shader_checks)

# ---- Desktop UI (slice UI-1): SDL2 window + Dear ImGui (MIT). Optional, OFF by default.
# Needs the SDL2 development files (Debian/Ubuntu: libsdl2-dev). Dear ImGui is
# downloaded at configure time from a pinned release (SHA-256 checked), or pass
# -DPNR_IMGUI_DIR=<unpacked imgui source> to build offline. Linux only.
option(PNR_BUILD_UI "Build the SDL2 + Dear ImGui desktop UI (pnr-ui)" OFF)
if(PNR_BUILD_UI)
  if(WIN32)
    message(FATAL_ERROR "The desktop UI is Linux only (Windows execution is Blocked).")
  endif()
  find_path(SDL2_INCLUDE_DIR NAMES SDL.h PATH_SUFFIXES SDL2)
  find_library(SDL2_LIBRARY NAMES SDL2 SDL2-2.0 libSDL2-2.0.so.0)
  if(NOT SDL2_INCLUDE_DIR OR NOT SDL2_LIBRARY)
    message(FATAL_ERROR "PNR_BUILD_UI needs SDL2 dev files (apt install libsdl2-dev), or pass -DSDL2_INCLUDE_DIR=... -DSDL2_LIBRARY=...")
  endif()
  if(PNR_IMGUI_DIR)
    set(PNR_IMGUI_SRC ${PNR_IMGUI_DIR})
  else()
    include(FetchContent)
    FetchContent_Declare(pnr_imgui
      URL https://github.com/ocornut/imgui/archive/refs/tags/v1.93.0.tar.gz
      URL_HASH SHA256=f7bf6bc76e5d79d40d751096663e6c5d18daf08129dd52e7f7b09e4251c8eeee)
    FetchContent_MakeAvailable(pnr_imgui)
    set(PNR_IMGUI_SRC ${pnr_imgui_SOURCE_DIR})
  endif()
  # Third-party code: built without our warning flags, not modified.
  add_library(pnr_imgui STATIC
    ${PNR_IMGUI_SRC}/imgui.cpp ${PNR_IMGUI_SRC}/imgui_draw.cpp ${PNR_IMGUI_SRC}/imgui_tables.cpp ${PNR_IMGUI_SRC}/imgui_widgets.cpp
    ${PNR_IMGUI_SRC}/backends/imgui_impl_sdl2.cpp ${PNR_IMGUI_SRC}/backends/imgui_impl_sdlrenderer2.cpp)
  target_include_directories(pnr_imgui PUBLIC ${PNR_IMGUI_SRC} ${SDL2_INCLUDE_DIR})
  target_compile_options(pnr_imgui PRIVATE -w)
  target_link_libraries(pnr_imgui PUBLIC ${SDL2_LIBRARY} ${CMAKE_DL_LIBS})
  add_executable(pnr-ui ui/pnr_ui_main.cpp)
  target_link_libraries(pnr-ui PRIVATE pnr_imgui pnr_ui_model)
  find_program(PNR_XVFB_RUN xvfb-run)
  if(PNR_XVFB_RUN)
    # Smoke: the real window opens on a virtual display, renders frames and saves a screenshot.
    add_test(NAME pnr_ui_smoke COMMAND ${PNR_XVFB_RUN} -a -s "-screen 0 1366x768x24"
             $<TARGET_FILE:pnr-ui> --db ${CMAKE_BINARY_DIR}/ui_smoke.db --frames 10 --shot 9:${CMAKE_BINARY_DIR}/ui_smoke.bmp)
    set_tests_properties(pnr_ui_smoke PROPERTIES PASS_REGULAR_EXPRESSION "saved .*ui_smoke.bmp" WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  else()
    message(STATUS "xvfb-run not found: pnr_ui_smoke test not registered")
  endif()
endif()
