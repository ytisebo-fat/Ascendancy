cmake_minimum_required(VERSION 3.16)
project(AscendancyNative C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# Core library: asset formats + LE executable image handling.
# Platform independent, builds on Windows and Linux (tools + tests).
# ---------------------------------------------------------------------------
add_library(asc_core STATIC
  src/common/png_write.cpp
  src/assets/cob.cpp
  src/assets/palette.cpp
  src/assets/shp.cpp
  src/assets/font.cpp
  src/le/le_image.cpp
)
target_include_directories(asc_core PUBLIC src)

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------
add_executable(ascx tools/ascx/main.cpp)
target_link_libraries(ascx PRIVATE asc_core)

add_executable(ledump tools/ledump/main.cpp)
target_link_libraries(ledump PRIVATE asc_core)

if(MINGW)
  # Single-file executables without MinGW runtime DLL dependencies.
  foreach(t ascx ledump)
    target_link_options(${t} PRIVATE -static -s)
  endforeach()
endif()

# ---------------------------------------------------------------------------
# The game runner (Windows, 32-bit only)
# ---------------------------------------------------------------------------
if(WIN32)
  if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "The game runner must be built as 32-bit (it runs the original 32-bit code)")
  endif()
  add_executable(Ascendancy
    src/runtime/main.cpp
    src/runtime/game_image.cpp
    src/runtime/dos.cpp
    src/runtime/patches.cpp
    src/runtime/hooks.cpp
    src/runtime/video.cpp
    src/runtime/vfx_driver.cpp
    src/runtime/settings.cpp
    src/runtime/options_ui.cpp
    src/runtime/camera.cpp
  )
  # SDL2 (static). Point SDL2_ROOT at the SDL2 MinGW development package's
  # i686-w64-mingw32 directory.
  set(SDL2_ROOT "" CACHE PATH "SDL2 MinGW dev root (…/SDL2-2.x/i686-w64-mingw32)")
  if(NOT SDL2_ROOT)
    message(FATAL_ERROR "Set -DSDL2_ROOT=<SDL2-2.x/i686-w64-mingw32>")
  endif()
  target_include_directories(Ascendancy PRIVATE ${SDL2_ROOT}/include/SDL2)
  target_link_libraries(Ascendancy PRIVATE asc_core ${SDL2_ROOT}/lib/libSDL2.a
    setupapi winmm imm32 version ole32 oleaut32 uuid gdi32 user32 shell32 advapi32 dinput8)
  # 16 MB stack: the game runs on the main thread's stack. Fixed image base
  # well above the low area the game image and DOS emulation use.
  target_link_options(Ascendancy PRIVATE -static -s -Wl,--stack,16777216 -Wl,--image-base,0x20000000)
endif()
