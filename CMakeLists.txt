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

if(POLY_TESTS)
  enable_testing()
  add_subdirectory(core/tests)
endif()

if(POLY_BENCH)
  add_subdirectory(bench)
endif()
