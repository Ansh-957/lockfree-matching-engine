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

# -----------------------------------------------------------------------------
# Compiler warnings — treat warnings seriously
# -----------------------------------------------------------------------------
add_compile_options(
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion
    -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
    -Woverloaded-virtual -Wsign-conversion
)

# Release builds: aggressive optimization for benchmarking
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -march=native -DNDEBUG)
endif()

# -----------------------------------------------------------------------------
# Dependencies via vcpkg (or system packages)
# -----------------------------------------------------------------------------
find_package(Boost REQUIRED COMPONENTS system)
find_package(simdjson CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(benchmark CONFIG REQUIRED)

# -----------------------------------------------------------------------------
# Core library — the matching engine itself
# -----------------------------------------------------------------------------
add_library(engine_core
    src/core/order_book.cpp
    src/core/matching_engine.cpp
)

target_include_directories(engine_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(engine_core PUBLIC
    spdlog::spdlog
)

# -----------------------------------------------------------------------------
# Feed library — WebSocket client + Coinbase parser
# -----------------------------------------------------------------------------
add_library(engine_feed
    src/feed/ws_client.cpp
    src/feed/coinbase_feed.cpp
    src/feed/feed_handler.cpp
)

target_include_directories(engine_feed PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(engine_feed PUBLIC
    engine_core
    Boost::system
    simdjson::simdjson
    spdlog::spdlog
    ssl
    crypto
)

# -----------------------------------------------------------------------------
# Metrics library
# -----------------------------------------------------------------------------
add_library(engine_metrics
    src/metrics/metrics_collector.cpp
    src/metrics/trade_logger.cpp
)

target_include_directories(engine_metrics PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(engine_metrics PUBLIC
    engine_core
    spdlog::spdlog
)

# -----------------------------------------------------------------------------
# Main executable
# -----------------------------------------------------------------------------
add_executable(matching-engine src/main.cpp)

target_link_libraries(matching-engine PRIVATE
    engine_core
    engine_feed
    engine_metrics
    pthread
)

# -----------------------------------------------------------------------------
# Synthetic data generator tool
# -----------------------------------------------------------------------------
add_executable(synthetic-generator tools/synthetic_generator.cpp)

target_link_libraries(synthetic-generator PRIVATE
    engine_core
)

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------
enable_testing()

add_executable(unit_tests
    test/memory_pool_test.cpp
    test/order_book_test.cpp
    test/matching_engine_test.cpp
    test/spsc_queue_test.cpp
    test/feed_parser_test.cpp
)

target_link_libraries(unit_tests PRIVATE
    engine_core
    engine_feed
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME unit_tests COMMAND unit_tests)

# -----------------------------------------------------------------------------
# Benchmarks
# -----------------------------------------------------------------------------
add_executable(order_book_bench bench/order_book_bench.cpp)
target_link_libraries(order_book_bench PRIVATE engine_core benchmark::benchmark benchmark::benchmark_main)

add_executable(ring_buffer_bench bench/ring_buffer_bench.cpp)
target_link_libraries(ring_buffer_bench PRIVATE engine_core benchmark::benchmark benchmark::benchmark_main)

add_executable(matching_bench bench/matching_bench.cpp)
target_link_libraries(matching_bench PRIVATE engine_core benchmark::benchmark benchmark::benchmark_main)

add_executable(memory_pool_bench bench/memory_pool_bench.cpp)
target_link_libraries(memory_pool_bench PRIVATE engine_core benchmark::benchmark benchmark::benchmark_main)
