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
# Sanitizers (ASan + UBSan). Enable with -DLOB_SANITIZE=ON for local use;
# the CI workflow enables this automatically in its sanitizer job.
# -----------------------------------------------------------------------------
option(LOB_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
if(LOB_SANITIZE AND NOT MSVC)
    set(SAN_FLAGS "-fsanitize=address,undefined -fno-omit-frame-pointer")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SAN_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${SAN_FLAGS}")
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

    add_executable(lob_fuzz
        tests/fuzz_cross_check.cpp
    )
    target_link_libraries(lob_fuzz PRIVATE lob_core GTest::gtest_main)

    add_executable(lob_spsc_tests
        tests/test_spsc_queue.cpp
    )
    target_link_libraries(lob_spsc_tests PRIVATE lob_core GTest::gtest_main)

    add_executable(lob_photon_tests
        tests/test_photon.cpp
    )
    target_link_libraries(lob_photon_tests PRIVATE lob_core GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(lob_tests)
    gtest_discover_tests(lob_fuzz)
    gtest_discover_tests(lob_spsc_tests)
    gtest_discover_tests(lob_photon_tests)
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

    add_executable(lob_ablation
        benchmarks/ablation_main.cpp
    )
    target_link_libraries(lob_ablation PRIVATE lob_core)

    add_executable(lob_replay
        benchmarks/replay_main.cpp
    )
    target_link_libraries(lob_replay PRIVATE lob_core)
endif()

# -----------------------------------------------------------------------------
# Live terminal demos / dashboard.
# -----------------------------------------------------------------------------
add_executable(lob_demo
    demos/demo_main.cpp
)
target_link_libraries(lob_demo PRIVATE lob_core)

add_executable(lob_dashboard
    demos/dashboard_main.cpp
)
target_link_libraries(lob_dashboard PRIVATE lob_core)

add_executable(lob_report
    tools/report_main.cpp
)
target_link_libraries(lob_report PRIVATE lob_core)
