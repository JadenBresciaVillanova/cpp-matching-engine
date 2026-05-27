cmake_minimum_required(VERSION 3.20)

project(lob
    VERSION 0.1.0
    DESCRIPTION "A cache-optimized limit order book and matching engine"
    LANGUAGES CXX)

# -----------------------------------------------------------------------------
# Standard. C++20 for concepts, std::span, better constexpr.
# Set per-target below rather than globally where it matters, but a project-wide
# default is fine here.
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    # Benchmarks are MEANINGLESS in Debug: no inlining, no optimization,
    # simdjson/templates crawl. Default to Release so a naive `cmake .. && make`
    # does not produce embarrassing numbers.
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
    message(STATUS "No build type set; defaulting to Release.")
endif()

# Put deps' noise aside.
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# -----------------------------------------------------------------------------
# The core library. Header-mostly today; .cpp files added as we implement.
# We expose it as a target with PUBLIC include dirs so consumers (tests,
# benchmarks, apps) inherit the include path correctly. This is the modern,
# target-based approach - no global include_directories(), no hardcoded vcpkg
# paths.
# -----------------------------------------------------------------------------
add_library(lob_core
    src/core/order_book.cpp
)
target_include_directories(lob_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_compile_features(lob_core PUBLIC cxx_std_20)

# Optimization flags applied ONLY to our targets, ONLY in optimized configs,
# and expressed per-compiler. -march=native lets the compiler use this machine's
# full instruction set (AVX2/BMI etc.). NOTE: a -march=native binary is NOT
# portable to older CPUs - fine for local benchmarking, document it for CI.
include(CheckCXXCompilerFlag)
if(NOT MSVC)
    target_compile_options(lob_core PRIVATE
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:Release>:-march=native>
        -Wall -Wextra)
else()
    # /O2 implies the useful optimizations. /GL (whole-program opt) does NOTHING
    # without /LTCG at link time, so if you enable one you must enable the other.
    target_compile_options(lob_core PRIVATE
        $<$<CONFIG:Release>:/O2>
        /W4)
endif()

# -----------------------------------------------------------------------------
# Tests (GoogleTest via FetchContent - no system install needed).
# -----------------------------------------------------------------------------
option(LOB_BUILD_TESTS "Build unit/property tests" ON)
if(LOB_BUILD_TESTS)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    add_executable(lob_tests
        tests/test_order_book.cpp
    )
    target_link_libraries(lob_tests PRIVATE lob_core GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(lob_tests)
endif()

# -----------------------------------------------------------------------------
# Benchmarks / replay-driver app.
# -----------------------------------------------------------------------------
option(LOB_BUILD_BENCH "Build the benchmark driver" ON)
if(LOB_BUILD_BENCH)
    add_executable(lob_bench
        benchmarks/bench_main.cpp
    )
    target_link_libraries(lob_bench PRIVATE lob_core)
endif()
