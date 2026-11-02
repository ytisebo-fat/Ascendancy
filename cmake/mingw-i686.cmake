# Cross-compile toolchain: 32-bit Windows using llvm-mingw.
# Usage: cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-i686.cmake
#
# The game runtime must be a 32-bit process: in Phase 1 the original 32-bit
# game code runs natively inside it. Windows 10/11 (x64 and ARM64) run 32-bit
# programs out of the box.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR i686)

set(TOOLCHAIN_PREFIX i686-w64-mingw32)
set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-clang)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-clang++)
set(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}-windres)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
