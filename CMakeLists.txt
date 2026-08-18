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
option(BUILD_BENCHMARKS "Build micro-benchmarks (Google Benchmark)"     OFF)
option(BUILD_FEED       "Build live feed pipeline (Boost/simdjson/TLS)" OFF)
option(BUILD_TOOLS      "Build synthetic generator / replay tools"      OFF)

# Warnings
if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(
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

# Feed library + main executable - Phase 3
if(BUILD_FEED)
    find_package(Boost REQUIRED COMPONENTS system)
    find_package(simdjson CONFIG REQUIRED)
    find_package(spdlog CONFIG REQUIRED)
    find_package(OpenSSL REQUIRED)

    add_library(engine_feed
        src/feed/ws_client.cpp
        src/feed/coinbase_feed.cpp
        src/feed/feed_handler.cpp
    )
    target_include_directories(engine_feed PUBLIC ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(engine_feed PUBLIC
        engine_core
        Boost::system
        simdjson::simdjson
        spdlog::spdlog
        OpenSSL::SSL
        OpenSSL::Crypto
    )

    add_library(engine_metrics
        src/metrics/metrics_collector.cpp
        src/metrics/trade_logger.cpp
    )
    target_include_directories(engine_metrics PUBLIC ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(engine_metrics PUBLIC engine_core spdlog::spdlog)

    add_executable(matching-engine src/main.cpp)
    target_link_libraries(matching-engine PRIVATE
        engine_core
        engine_feed
        engine_metrics
    )
    if(NOT MSVC)
        target_link_libraries(matching-engine PRIVATE pthread)
    endif()
endif()

# Tools - Phase 2 (Commit 7)
if(BUILD_TOOLS)
    add_executable(synthetic-generator tools/synthetic_generator.cpp)
    target_link_libraries(synthetic-generator PRIVATE engine_core)
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
        # test/order_book_test.cpp       # Commit 4
        # test/spsc_queue_test.cpp       # Commit 5
        # test/matching_engine_test.cpp  # Commit 6
        # test/feed_parser_test.cpp      # Commit 9
    )

    target_link_libraries(unit_tests PRIVATE
        engine_core
        GTest::gtest
        GTest::gtest_main
    )

    add_test(NAME unit_tests COMMAND unit_tests)
endif()

# Benchmarks - Phase 2 (Commit 7)
if(BUILD_BENCHMARKS)
    find_package(benchmark CONFIG REQUIRED)

    foreach(bench_name order_book_bench ring_buffer_bench matching_bench memory_pool_bench)
        add_executable(${bench_name} bench/${bench_name}.cpp)
        target_link_libraries(${bench_name} PRIVATE
            engine_core
            benchmark::benchmark
            benchmark::benchmark_main
        )
    endforeach()
endif()
