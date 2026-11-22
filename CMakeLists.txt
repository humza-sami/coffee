cmake_minimum_required(VERSION 3.25)
project(coffee VERSION 0.1.0 LANGUAGES C CXX)
set(COFFEE_VERSION_SUFFIX "-beta.1")  # numbered beta prerelease; keep equal to kVersion in include/coffee/util.hpp
set(COFFEE_VERSION_FULL "${PROJECT_VERSION}${COFFEE_VERSION_SUFFIX}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)  # portable: no -march=native
endif()

include(cmake/Dependencies.cmake)

add_library(coffee_core STATIC
  src/util.cpp
  src/model.cpp
  src/store.cpp
  src/artifacts.cpp
  src/executor.cpp
  src/workspace.cpp
  src/protocol.cpp
  src/server.cpp
  src/server_jobs.cpp
  src/server_leases.cpp
  src/client.cpp
  src/worker.cpp
  src/worker_attempt.cpp
  src/cli.cpp
  src/cli_net.cpp)
target_include_directories(coffee_core PUBLIC include)
target_link_libraries(coffee_core PUBLIC coffee_third_party_json coffee_httplib coffee_sqlite yaml-cpp::yaml-cpp)
target_compile_options(coffee_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow)

add_executable(coffee src/main.cpp)
target_link_libraries(coffee PRIVATE coffee_core)
target_compile_options(coffee PRIVATE -Wall -Wextra -Wpedantic -Wshadow)

option(COFFEE_BUILD_TESTS "Build unit tests" ON)
if(COFFEE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

include(cmake/Install.cmake)

# Benchmark workload (opt-out); built next to the tests, never installed.
option(COFFEE_BUILD_BENCH "Build the benchmark workload helper" ON)
if(COFFEE_BUILD_BENCH)
  add_executable(coffee_bench_work bench/work.cpp)
  target_compile_options(coffee_bench_work PRIVATE -Wall -Wextra -Wpedantic)
endif()
