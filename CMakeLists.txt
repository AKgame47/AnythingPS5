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
add_library(pnr_loader loader/elf_parser.cpp loader/native_loader.cpp loader/nid.cpp)

add_library(pnr_prx prx/prx.cpp prx/prx_file.cpp loader/import_resolver.cpp)

add_library(pnr_psabi SHARED psabi/psabi.cpp)
target_link_libraries(pnr_psabi PRIVATE pnr_compat)
set_target_properties(pnr_psabi PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(pnr_prx PUBLIC pnr_loader pnr_compat)
target_link_libraries(pnr_loader PUBLIC pnr_core)

# MODULE 03 - compatibility services
add_library(pnr_shader shader/rdna.cpp shader/translate.cpp shader/loop_memory.cpp)
target_include_directories(pnr_shader PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(pnr_graphics graphics/vulkan.cpp graphics/render.cpp graphics/spirv.cpp)
target_include_directories(pnr_graphics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pnr_graphics PUBLIC pnr_core dl)
target_link_libraries(pnr_shader PUBLIC pnr_graphics)
set_target_properties(pnr_graphics PROPERTIES POSITION_INDEPENDENT_CODE ON)
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
add_library(pnr_services services/virtual_fs.cpp services/audio.cpp services/input.cpp)
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

# CLI launcher (STEP 1-10 workflow driver)
add_executable(pnr apps/pnr_cli.cpp)
target_link_libraries(pnr PRIVATE pnr_core pnr_loader pnr_compat pnr_package pnr_library pnr_platform pnr_ape pnr_laptop pnr_services pnr_prx pnr_graphics)

# Dev tooling: build .pnrgame packages
add_executable(pnr_make_package tools/make_package.cpp)
target_link_libraries(pnr_make_package PRIVATE pnr_package pnr_library pnr_core)

# Developer-controlled native test application ("demo game" v0)
add_executable(pnr_test_app examples/test_app.cpp)
add_executable(ps_demo examples/ps_demo.cpp)
target_link_libraries(ps_demo PRIVATE pnr_psabi)
set_target_properties(ps_demo PROPERTIES BUILD_RPATH "$ORIGIN")
target_link_libraries(pnr_test_app PRIVATE pnr_compat pnr_core)

enable_testing()
add_executable(pnr_tests tests/test_main.cpp tests/loop_tests.cpp tests/loop_memory_tests.cpp tests/loop_copy_tests.cpp tests/render_tests.cpp tests/cube_tests.cpp)

# Let the tests launch the real PS-style demo binary.
target_compile_definitions(pnr_tests PRIVATE
  PNR_PS_DEMO_PATH="$<TARGET_FILE:ps_demo>"
  PNR_PSABI_DIR="$<TARGET_FILE_DIR:pnr_psabi>")
target_link_libraries(pnr_tests PRIVATE pnr_core pnr_loader pnr_compat pnr_package pnr_library pnr_platform pnr_ape pnr_laptop pnr_services pnr_prx pnr_graphics pnr_shader)

add_test(NAME pnr_unit_tests COMMAND pnr_tests)
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
