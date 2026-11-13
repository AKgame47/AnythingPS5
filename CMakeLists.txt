# PNR - PlayStation Native Runtime (AnythingPS5)
# Research/prototype build. C++20 core; Rust crates are planned but no
# Rust toolchain exists in the current dev sandbox (documented, not faked).
cmake_minimum_required(VERSION 3.16)
project(pnr_runtime VERSION 0.1.0 LANGUAGES CXX)

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
add_library(pnr_loader loader/elf_parser.cpp loader/native_loader.cpp)
target_link_libraries(pnr_loader PUBLIC pnr_core)

# MODULE 03 - compatibility services
add_library(pnr_compat compat/services.cpp)
target_link_libraries(pnr_compat PUBLIC pnr_core)

# MODULE 07 - host OS abstraction (Linux backend; Windows backend planned)
add_library(pnr_platform platform/host_linux.cpp)
target_link_libraries(pnr_platform PUBLIC pnr_core)

# Installer - .pnrgame package format + controlled importer
add_library(pnr_package package/sha256.cpp package/tar.cpp package/importer.cpp)
target_link_libraries(pnr_package PUBLIC pnr_core pnr_loader)

# MODULE 09 - game library metadata (SQLite)
# The dev sandbox ships libsqlite3.so without dev headers; the repo carries
# a minimal API shim (library/sqlite3_shim.h). With real headers installed,
# add -DPNR_USE_SYSTEM_SQLITE3 and link the normal way.
find_library(SQLITE3_LIBRARY NAMES sqlite3 libsqlite3.so.0)
if(NOT SQLITE3_LIBRARY)
  message(FATAL_ERROR "libsqlite3 runtime not found; library module cannot build")
endif()
add_library(pnr_library library/game_db.cpp)
target_link_libraries(pnr_library PUBLIC pnr_core ${SQLITE3_LIBRARY})

find_package(Threads REQUIRED)
target_link_libraries(pnr_compat PUBLIC Threads::Threads)
target_link_libraries(pnr_loader PUBLIC Threads::Threads pnr_platform)
target_link_libraries(pnr_package PUBLIC pnr_library)

# CLI launcher (STEP 1-10 workflow driver)
add_executable(pnr apps/pnr_cli.cpp)
target_link_libraries(pnr PRIVATE pnr_core pnr_loader pnr_compat pnr_package pnr_library pnr_platform)

# Dev tooling: build .pnrgame packages
add_executable(pnr_make_package tools/make_package.cpp)
target_link_libraries(pnr_make_package PRIVATE pnr_package pnr_library pnr_core)

# Developer-controlled native test application ("demo game" v0)
add_executable(pnr_test_app examples/test_app.cpp)
target_link_libraries(pnr_test_app PRIVATE pnr_compat pnr_core)

enable_testing()
add_executable(pnr_tests tests/test_main.cpp)
target_link_libraries(pnr_tests PRIVATE pnr_core pnr_loader pnr_compat pnr_package pnr_library pnr_platform)

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
