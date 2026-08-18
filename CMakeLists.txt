cmake_minimum_required(VERSION 3.20)
project(matching-engine
    VERSION 0.1.0
    LANGUAGES CXX
    DESCRIPTION "Low-latency matching engine & market data pipeline"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build options - flip ON as each phase is implemented. Keeping later-phase
# targets OFF means the project builds with no dependencies beyond GTest
# until Phase 3
option(BUILD_TESTS      "Build unit tests (GoogleTest)"                 ON)
option(BUILD_BENCHMARKS "Build micro-benchmarks (Google Benchmark)"     ON)
option(BUILD_FEED       "Build live feed pipeline (Boost/simdjson/TLS)" OFF)
option(BUILD_TOOLS      "Build synthetic generator / replay tools"      ON)

# Warnings - attached per-target (not add_compile_options) so they apply to
# OUR code only; FetchContent'd dependencies (GTest, Google Benchmark) build
# with their own warning sets, and benchmark's -Werror would otherwise turn
# our extra -Wconversion warnings in ITS code into build failures
add_library(engine_warnings INTERFACE)
if(MSVC)
    # C4324 warns that alignas() padded a struct - that padding is the whole
    # point of cache-line alignment, so the warning is noise here
    target_compile_options(engine_warnings INTERFACE /W4 /permissive- /wd4324)
else()
    target_compile_options(engine_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion
        -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
        -Woverloaded-virtual -Wsign-conversion
    )
endif()

# Release optimization (GCC/Clang only; MSVC Release already uses /O2)
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT MSVC)
    add_compile_options(-O3 -march=native)
endif()

# Core library - the matching engine itself, no external dependencies
add_library(engine_core
    src/core/order_book.cpp
    src/core/matching_engine.cpp
)

target_include_directories(engine_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(engine_core PRIVATE engine_warnings)

# Feed library - Phase 3, built commit by commit
if(BUILD_FEED)
    # Beast/Asio are header-only; OpenSSL provides the TLS implementation
    find_package(Boost REQUIRED)
    find_package(OpenSSL REQUIRED)
    find_package(Threads REQUIRED)

    add_library(engine_feed
        src/feed/ws_client.cpp
        # src/feed/coinbase_feed.cpp     # Commit 9 (needs simdjson)
        # src/feed/feed_handler.cpp      # Commit 9
    )
    target_include_directories(engine_feed PUBLIC ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(engine_feed PRIVATE engine_warnings)
    target_link_libraries(engine_feed PUBLIC
        engine_core
        Boost::headers
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
    )

    # Commit 8 verification: live handshake against the Coinbase feed
    add_executable(ws-smoke tools/ws_client_smoke.cpp)
    target_link_libraries(ws-smoke PRIVATE engine_feed engine_warnings)

    # Commit 10/11: main executable + metrics library
    # add_library(engine_metrics
    #     src/metrics/metrics_collector.cpp
    #     src/metrics/trade_logger.cpp
    # )
    # add_executable(matching-engine src/main.cpp)
endif()

# Tools - Phase 2 (Commit 7)
if(BUILD_TOOLS)
    add_executable(synthetic-generator tools/synthetic_generator.cpp)
    target_link_libraries(synthetic-generator PRIVATE engine_core engine_warnings)
endif()

# Tests - sources are added commit by commit as components are finished
if(BUILD_TESTS)
    enable_testing()

    # prefer a vcpkg/system GTest; fall back to downloading it at configure
    # time so tests build with zero setup
    find_package(GTest CONFIG QUIET)
    if(NOT GTest_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        # on Windows, keep GTest's CRT linkage consistent with our targets
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()

    add_executable(unit_tests
        test/memory_pool_test.cpp
        test/order_book_test.cpp
        test/spsc_queue_test.cpp
        test/matching_engine_test.cpp
        # test/feed_parser_test.cpp      # Commit 9
    )

    target_link_libraries(unit_tests PRIVATE
        engine_core
        engine_warnings
        GTest::gtest
        GTest::gtest_main
    )

    add_test(NAME unit_tests COMMAND unit_tests)
endif()

# Benchmarks - Phase 2 (Commit 7)
if(BUILD_BENCHMARKS)
    # prefer a vcpkg/system Google Benchmark; fall back to downloading it at
    # configure time (same pattern as GTest above)
    find_package(benchmark CONFIG QUIET)
    if(NOT benchmark_FOUND)
        include(FetchContent)
        set(BENCHMARK_ENABLE_TESTING     OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_INSTALL     OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_WERROR      OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            googlebenchmark
            URL https://github.com/google/benchmark/archive/refs/tags/v1.9.0.zip
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        FetchContent_MakeAvailable(googlebenchmark)
    endif()

    find_package(Threads REQUIRED)

    foreach(bench_name order_book_bench ring_buffer_bench matching_bench memory_pool_bench)
        add_executable(${bench_name} bench/${bench_name}.cpp)
        # CMAKE_SOURCE_DIR so benchmarks can include "tools/synthetic_workload.h"
        target_include_directories(${bench_name} PRIVATE ${CMAKE_SOURCE_DIR})
        target_link_libraries(${bench_name} PRIVATE
            engine_core
            engine_warnings
            benchmark::benchmark
            benchmark::benchmark_main
            Threads::Threads
        )
    endforeach()
endif()
