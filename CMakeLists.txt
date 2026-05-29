cmake_minimum_required(VERSION 3.20)
project(NanoExchange LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Release by default. The whole point of this project is measured performance,
# and a debug build is an order of magnitude slower with a different shape.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Warnings on everywhere.
add_compile_options(-Wall -Wextra -Wpedantic)

# Optimise hard for release. -march=native is guarded because Apple clang on
# arm64 does not always accept it. We fall back to -mcpu=native, then to nothing.
include(CheckCXXCompilerFlag)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    check_cxx_compiler_flag("-march=native" NANO_HAS_MARCH_NATIVE)
    check_cxx_compiler_flag("-mcpu=native"  NANO_HAS_MCPU_NATIVE)
    if(NANO_HAS_MARCH_NATIVE)
        add_compile_options(-march=native)
    elseif(NANO_HAS_MCPU_NATIVE)
        add_compile_options(-mcpu=native)
    endif()
    add_compile_options(-O3)

    # Link time optimisation recovers cross translation unit inlining, which
    # matters because the matching code lives in a separate .cpp from its callers.
    include(CheckIPOSupported)
    check_ipo_supported(RESULT NANO_HAS_IPO OUTPUT _ipo_msg)
    if(NANO_HAS_IPO)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    endif()
endif()

# Core matching library.
add_library(nano_core
    src/order_book.cpp
    src/matching_engine.cpp)
target_include_directories(nano_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Demo and market simulation.
add_executable(nano_exchange src/main.cpp)
target_link_libraries(nano_exchange PRIVATE nano_core)

# Options let a bare configure skip the fetched dependencies when wanted.
option(NANO_BUILD_TESTS "Build the test suite"   ON)
option(NANO_BUILD_BENCH "Build the benchmarks"   ON)

include(FetchContent)

if(NANO_BUILD_TESTS)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    enable_testing()
    add_subdirectory(test)
endif()

if(NANO_BUILD_BENCH)
    set(BENCHMARK_ENABLE_TESTING   OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL   OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(googlebenchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        v1.8.3)
    FetchContent_MakeAvailable(googlebenchmark)
    add_subdirectory(bench)
endif()
