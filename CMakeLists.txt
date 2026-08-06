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

# The logger runs on its own thread and LockQueue uses std::mutex/condition_variable.
find_package(Threads REQUIRED)

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
add_library(me_core
    src/me/BookLevel.cpp
    src/me/Order.cpp
    src/me/OrderManager.cpp
    src/me/OrderBook.cpp
)
# PUBLIC because MatchingEngine.h (a me_core header) includes RiskManager.h, so
# every consumer of me_core needs the risk_manager directory on its include path.
target_include_directories(me_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include/me
    ${CMAKE_CURRENT_SOURCE_DIR}/include/risk_manager
)
target_link_libraries(me_core PRIVATE project_warnings project_options)

# -----------------------------------------------------------------------------
# Main executable
# -----------------------------------------------------------------------------
add_executable(me_main src/main.cpp)
target_link_libraries(me_main PRIVATE
    me_core
    me_ds_lock          # LockQueue
    me_event_handler    # Logger
    Threads::Threads
    project_warnings
    project_options
)
 
# -----------------------------------------------------------------------------
# Lock-based data structures (practice) — header-only.
# Headers live in include/data_structures/; there is no .cpp, so this is an
# INTERFACE target. Link it from anything that includes lock_queue.h.
# (The old compile-check TU is gone; add one back and switch this to STATIC if
# you want the build to force template instantiation again.)
# -----------------------------------------------------------------------------
add_library(me_ds_lock INTERFACE)
target_include_directories(me_ds_lock INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include/data_structures)

# -----------------------------------------------------------------------------
# Event handler (Logger) — header-only, consumes the event queue off the hot path.
# -----------------------------------------------------------------------------
add_library(me_event_handler INTERFACE)
target_include_directories(me_event_handler INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include/event_handler)
target_link_libraries(me_event_handler INTERFACE me_core Threads::Threads)

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
 
# -----------------------------------------------------------------------------
# Benchmarks
# -----------------------------------------------------------------------------
add_subdirectory(bench)