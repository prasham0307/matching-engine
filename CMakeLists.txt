cmake_minimum_required(VERSION 3.20)
project(matching-engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Per-config flags. Applied only to our targets, not to fetched dependencies.
add_library(lob_flags INTERFACE)
target_compile_options(lob_flags INTERFACE
  $<$<CONFIG:Debug>:-Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer>
  $<$<CONFIG:Release>:-O3>)
target_link_options(lob_flags INTERFACE
  $<$<CONFIG:Debug>:-fsanitize=address,undefined>)

include(FetchContent)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(benchmark
  URL https://github.com/google/benchmark/archive/refs/tags/v1.8.3.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(googletest benchmark)

add_library(lob STATIC src/placeholder.cpp src/naive_matcher.cpp src/order_book.cpp src/matching_engine.cpp src/replay.cpp)
target_include_directories(lob PUBLIC include)
target_link_libraries(lob PUBLIC lob_flags)

enable_testing()
add_subdirectory(tests)
add_subdirectory(bench)
add_subdirectory(tools)
