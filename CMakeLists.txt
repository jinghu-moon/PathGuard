cmake_minimum_required(VERSION 3.24)
project(folder_manager_rule_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
)
FetchContent_Declare(
  googlebenchmark
  URL https://github.com/google/benchmark/archive/refs/tags/v1.9.4.zip
)
FetchContent_MakeAvailable(googletest googlebenchmark)

enable_testing()

add_library(fm_rule_engine
  native/daemon_utils.cpp
  native/path_mapper.cpp
  native/path_kind_cache.cpp
  native/rule_config.cpp
  native/rule_engine.cpp
)

target_include_directories(fm_rule_engine PUBLIC native)

target_compile_features(fm_rule_engine PUBLIC cxx_std_20)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(fm_rule_engine PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(fm_rule_engine_tests
  tests/parser_tests.cpp
  tests/compiler_tests.cpp
  tests/rule_config_tests.cpp
  tests/path_kind_cache_tests.cpp
  tests/matcher_tests.cpp
  tests/edge_tests.cpp
  tests/daemon_utils_tests.cpp
  tests/stats_tests.cpp
  tests/realworld_tests.cpp
)

target_link_libraries(fm_rule_engine_tests PRIVATE fm_rule_engine GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(fm_rule_engine_tests)

add_executable(fm_rule_cli
  tools/rule_cli.cpp
)

target_link_libraries(fm_rule_cli PRIVATE fm_rule_engine)
add_dependencies(fm_rule_engine_tests fm_rule_cli)

set(FM_RULE_CLI_TEST_RULES "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/rules_cli.ini")
add_test(NAME fm_rule_cli_smoke
  COMMAND $<TARGET_FILE:fm_rule_cli>
    --rules "${FM_RULE_CLI_TEST_RULES}"
    --process com.tencent.mm
    --path /storage/emulated/0/DCIM/A-TEST
    --op open
)

add_executable(fm_rule_engine_bench
  benchmarks/rule_engine_bench.cpp
)

target_link_libraries(fm_rule_engine_bench PRIVATE fm_rule_engine benchmark::benchmark)
