cmake_minimum_required(VERSION 3.20)

project(electronic_order_book LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(
            FATAL_ERROR
            "This project currently depends on Winsock and Windows APIs."
    )
endif()

# ============================================================
# CORE ENGINE
# ============================================================

set(ENGINE_SOURCES
        src/order_book.cpp
        src/market_event.cpp
        src/lobster_loader.cpp
        src/binary_codec.cpp
        src/replay_engine.cpp
        src/risk_management.cpp
        src/validation.cpp
)

# ============================================================
# SHARED SUPPORT
# ============================================================

set(SHARED_SCENARIO_SOURCES
        support/scenario_defaults.cpp
)

# ============================================================
# TEST SUPPORT AND TEST SUITES
# ============================================================

set(TEST_SUPPORT_SOURCES
        tests/test_support.cpp
)

set(TEST_SOURCES
        tests/order_book_tests.cpp
        tests/replay_tests.cpp
        tests/strategy_tests.cpp
        tests/pnl_tests.cpp
        tests/recovery_tests.cpp
)

# This file contains the main() function for engine_tests only.
set(TEST_MAIN_SOURCE
        tests/all_tests_main.cpp
)

# ============================================================
# DEMOS
# ============================================================

set(DEMO_SOURCES
        demos/strategy_pnl_demo.cpp
        demos/udp_recovery_demo.cpp
        demos/lobster_demo.cpp
)

# ============================================================
# BENCHMARKS
# ============================================================

set(BENCHMARK_SOURCES
        benchmarks/benchmark_utils.cpp
        benchmarks/matching_engine_benchmark.cpp
        benchmarks/replay_scalability.cpp
        benchmarks/udp_scalability.cpp
)

# ============================================================
# COMMON TARGET CONFIGURATION
# ============================================================

function(configure_order_book_target target_name)

    target_include_directories(
            ${target_name}
            PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/support
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
            ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks
    )

    target_compile_definitions(
            ${target_name}
            PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
    )

    if(MSVC)
        target_compile_options(
                ${target_name}
                PRIVATE
                /W4
                /permissive-
        )
    elseif(MINGW)
        target_compile_options(
                ${target_name}
                PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
    endif()

    target_link_libraries(
            ${target_name}
            PRIVATE
            ws2_32
            psapi
    )

endfunction()

# ============================================================
# FULL APPLICATION
# ============================================================
#
# This executable uses src/main.cpp as its only main() function.
# It includes the engine, tests, demos and benchmarks because
# src/main.cpp selects between them using command-line arguments.
#
# Do not add tests/all_tests_main.cpp to this target.

add_executable(
        electronic_order_book

        src/main.cpp

        ${ENGINE_SOURCES}
        ${SHARED_SCENARIO_SOURCES}
        ${TEST_SUPPORT_SOURCES}
        ${TEST_SOURCES}
        ${DEMO_SOURCES}
        ${BENCHMARK_SOURCES}
)

configure_order_book_target(
        electronic_order_book
)

# ============================================================
# TEST-ONLY EXECUTABLE
# ============================================================
#
# This executable uses tests/all_tests_main.cpp as its only
# main() function. It does not include demos or benchmarks.

add_executable(
        engine_tests

        ${TEST_MAIN_SOURCE}

        ${ENGINE_SOURCES}
        ${SHARED_SCENARIO_SOURCES}
        ${TEST_SUPPORT_SOURCES}
        ${TEST_SOURCES}
)

configure_order_book_target(
        engine_tests
)

# ============================================================
# CTEST
# ============================================================

enable_testing()

add_test(
        NAME engine_tests
        COMMAND engine_tests
)