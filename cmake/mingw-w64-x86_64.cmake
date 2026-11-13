# Cross-compile toolchain: Linux host -> Windows x86_64 (llvm-mingw).
# Get the toolchain: https://github.com/mstorsjo/llvm-mingw/releases
# Use: PATH=<llvm-mingw>/bin:$PATH cmake -S . -B build-win \
#   -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
#   -DPNR_SQLITE3_AMALGAMATION=/path/to/sqlite-amalgamation
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-clang)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-clang++)
# Static libstdc++/libgcc so the .exe runs on a stock Windows install.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
