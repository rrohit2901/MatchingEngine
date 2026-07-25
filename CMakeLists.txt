# Matching Engine — CMake setup
# Modern CMake (target-based). Tested with CMake 3.20+.
 
cmake_minimum_required(VERSION 3.20)
project(matching_engine
    VERSION 0.1.0
    DESCRIPTION "Lock Free single-symbol matching engine"
    LANGUAGES CXX
)
 
# -----------------------------------------------------------------------------
# Global C++ settings
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)  # use -std=c++20, not -std=gnu++20
 
# Default to Release if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
 
# Compile commands json for clangd / IDE integration
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
 
# -----------------------------------------------------------------------------
# Warning flags applied via an interface target (modern pattern)
# Link any target against `project_warnings` to inherit these.
# -----------------------------------------------------------------------------
add_library(project_warnings INTERFACE)
target_compile_options(project_warnings INTERFACE
    -Wall -Wextra -Wpedantic
    -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
    -Wunused -Woverloaded-virtual -Wconversion -Wsign-conversion
    -Wmisleading-indentation -Wnull-dereference -Wdouble-promotion
    -Wformat=2
)
 
# Performance flags for Release (Debug already has -O0 -g by default)
add_library(project_options INTERFACE)
target_compile_options(project_options INTERFACE
    $<$<CONFIG:Release>:-O3 -march=native -DNDEBUG>
    $<$<CONFIG:Debug>:-O0 -g3 -fno-omit-frame-pointer>
)
 
# -----------------------------------------------------------------------------
# Dependencies via FetchContent (no system installs required)
# -----------------------------------------------------------------------------
include(FetchContent)
 
# GoogleTest
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
 
# Google Benchmark
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.8.3
)
 
FetchContent_MakeAvailable(googletest benchmark)
 
# -----------------------------------------------------------------------------
# Core library
# -----------------------------------------------------------------------------
add_library(me_core src/BookLevel.cpp src/OrderBook.cpp src/Order.cpp src/Matcher.cpp src/MatchingEngine.cpp)
target_include_directories(me_core PUBLIC include/me)
target_link_libraries(me_core PRIVATE project_warnings project_options)
 
# Header-only stub so the empty library builds; remove when you add src files.
set_target_properties(me_core PROPERTIES LINKER_LANGUAGE CXX)
 
# -----------------------------------------------------------------------------
# Main executable
# -----------------------------------------------------------------------------
add_executable(me_main src/main.cpp)
target_link_libraries(me_main PRIVATE me_core project_warnings project_options)
 
# -----------------------------------------------------------------------------
# Lock-based data structures (practice) — compile-check only
# Forces instantiation of the LockQueue template so build surfaces its errors.
# -----------------------------------------------------------------------------
add_library(me_ds_lock STATIC me_ds_lock/lock_queue_compile_check.cpp)
target_include_directories(me_ds_lock PUBLIC me_ds_lock)
target_link_libraries(me_ds_lock PRIVATE project_warnings project_options)

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
 
# -----------------------------------------------------------------------------
# Benchmarks
# -----------------------------------------------------------------------------
add_subdirectory(bench)