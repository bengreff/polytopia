cmake_minimum_required(VERSION 3.20)
project(polytopia LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(POLY_TESTS "Build the test binary" ON)
option(POLY_BENCH "Build the benchmark binary" ON)
option(POLY_WERROR "Treat warnings as errors" ON)
option(POLY_ASAN "Build with address + UB sanitizers" OFF)

add_library(poly_flags INTERFACE)

# Float determinism across macOS ARM64, Windows x64 and WASM. See docs/rules-research/
# resolutions.md R14. The damage formula genuinely needs floats -- Knight attack is 3.5,
# Cloak defence 0.5 -- and IEEE-754 basic ops reproduce across those three targets ONLY if
# the compiler may not contract a*b+c into an FMA and may not reorder under fast-math.
# Without this the browser build and the trainer can compute different damage from the same
# position, which is the exact divergence README.md §4 calls "the project is already broken".
target_compile_options(poly_flags INTERFACE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-ffp-contract=off>
  $<$<CXX_COMPILER_ID:MSVC>:/fp:precise>
)

target_compile_options(poly_flags INTERFACE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wshadow -Wconversion>
  $<$<AND:$<BOOL:${POLY_WERROR}>,$<CXX_COMPILER_ID:AppleClang,Clang,GNU>>:-Werror>
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
  $<$<AND:$<BOOL:${POLY_WERROR}>,$<CXX_COMPILER_ID:MSVC>>:/WX>
)

if(POLY_ASAN)
  target_compile_options(poly_flags INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(poly_flags INTERFACE -fsanitize=address,undefined)
endif()

add_subdirectory(core)
add_subdirectory(agents)
add_subdirectory(cli)

if(POLY_TESTS)
  enable_testing()
  add_subdirectory(core/tests)
endif()

if(POLY_BENCH)
  add_subdirectory(bench)
endif()
