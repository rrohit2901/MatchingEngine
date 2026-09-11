# Matching Engine — CMake setup
# Modern CMake (target-based). Tested with CMake 3.20+.
 
cmake_minimum_required(VERSION 3.20)
project(matching_engine
    VERSION 0.1.0
    DESCRIPTION "Single-symbol limit order book matching engine"
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
# Build options. Every default reproduces the historical build exactly, so
#   cmake -S . -B build && cmake --build build && ctest --test-dir build
# is unaffected by their existence. The wheel build (scikit-build-core, see
# pyproject.toml) turns tests/bench/apps off and python on, so `pip install .`
# never clones googletest or benchmark.
# -----------------------------------------------------------------------------
option(ME_BUILD_TESTS  "Build the GoogleTest unit tests"                  ON)
option(ME_BUILD_BENCH  "Build the Google Benchmark suites"                ON)
option(ME_BUILD_APPS   "Build the me_main demo executable"                ON)
option(ME_BUILD_PYTHON "Build the pybind11 extension module"              OFF)
option(ME_NATIVE_ARCH  "Tune Release builds for this CPU (-march=native)" ON)

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
# -march=native is behind ME_NATIVE_ARCH: a binary built with it dies with SIGILL
# on any machine older than the one that built it, which is fine locally and fatal
# for a redistributable wheel. With the option ON (the default) this expands
# exactly as it always did.
add_library(project_options INTERFACE)
target_compile_options(project_options INTERFACE
    $<$<CONFIG:Release>:-O3 $<$<BOOL:${ME_NATIVE_ARCH}>:-march=native> -DNDEBUG>
    $<$<CONFIG:Debug>:-O0 -g3 -fno-omit-frame-pointer>
)
 
# -----------------------------------------------------------------------------
# Dependencies via FetchContent (no system installs required)
# -----------------------------------------------------------------------------
include(FetchContent)

# The logger runs on its own thread and LockQueue uses std::mutex/condition_variable.
find_package(Threads REQUIRED)

# Each dependency is fetched only when the thing that needs it is being built, so
# a Python-only configure (pip install .) clones neither. benchmark does not need
# googletest here because BENCHMARK_ENABLE_TESTING is forced OFF below.

# GoogleTest
if(ME_BUILD_TESTS)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
    )
    FetchContent_MakeAvailable(googletest)
endif()

# Google Benchmark
if(ME_BUILD_BENCH)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        v1.8.3
    )
    FetchContent_MakeAvailable(benchmark)
endif()

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

# The Python extension is a shared object, so every object linked into it must be
# position independent; without this the module fails to link on x86-64 with
# R_X86_64_32S relocation errors. Set unconditionally rather than behind
# ME_BUILD_PYTHON so there is one build of me_core instead of a PIC and a non-PIC
# variant that can drift apart — the hot path has no globals, so the extra
# indirection does not show up in bench/.
set_target_properties(me_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# -----------------------------------------------------------------------------
# Main executable
# -----------------------------------------------------------------------------
if(ME_BUILD_APPS)
    add_executable(me_main src/main.cpp)
    target_link_libraries(me_main PRIVATE
        me_core
        me_ds_lockfree      # SPSC RingBuffer behind EventQueue.h
        me_event_handler    # Logger
        Threads::Threads
        project_warnings
        project_options
    )
endif()

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
# Lock-free data structures — header-only (SPSC ring buffer).
# This no longer needs a compile-check TU to force instantiation: EventQueue.h
# instantiates RingBuffer<EventVariant, 128> for the logger pipeline, so
# anything building me_main / bench_latency type-checks the header for real.
# -----------------------------------------------------------------------------
add_library(me_ds_lockfree INTERFACE)
target_include_directories(me_ds_lockfree INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include/data_structures)
target_link_libraries(me_ds_lockfree INTERFACE Threads::Threads)

# -----------------------------------------------------------------------------
# Event handler (Logger) — header-only, consumes the event queue off the hot path.
# -----------------------------------------------------------------------------
add_library(me_event_handler INTERFACE)
target_include_directories(me_event_handler INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include/event_handler)
target_link_libraries(me_event_handler INTERFACE me_core me_ds_lockfree Threads::Threads)

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------
if(ME_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# -----------------------------------------------------------------------------
# Benchmarks
# -----------------------------------------------------------------------------
if(ME_BUILD_BENCH)
    add_subdirectory(bench)
endif()

# -----------------------------------------------------------------------------
# Python bindings (opt-in; scikit-build-core sets ME_BUILD_PYTHON=ON)
# Last in the file on purpose: the module links me_ds_lock and me_event_handler,
# and unlike me_main a subdirectory cannot forward-reference targets that do not
# exist yet.
# -----------------------------------------------------------------------------
if(ME_BUILD_PYTHON)
    add_subdirectory(src/python)
endif()