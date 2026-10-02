# src/python/CMakeLists.txt
# The pybind11 extension module. Configured only when ME_BUILD_PYTHON=ON.

# pybind11 normally comes from the pip package that scikit-build-core places on
# CMAKE_PREFIX_PATH. The FetchContent branch covers a bare developer configure
# (cmake -S . -B build -DME_BUILD_PYTHON=ON) on a machine with no pip build
# environment, where find_package would otherwise fail outright.
find_package(pybind11 CONFIG QUIET)
if(NOT pybind11_FOUND)
    message(STATUS "pybind11 not found on CMAKE_PREFIX_PATH - fetching v2.13.6")
    include(FetchContent)
    FetchContent_Declare(
        pybind11
        GIT_REPOSITORY https://github.com/pybind/pybind11.git
        GIT_TAG        v2.13.6
    )
    FetchContent_MakeAvailable(pybind11)
endif()

pybind11_add_module(_core MODULE module.cpp replay_bindings.cpp)
target_compile_features(_core PRIVATE cxx_std_20)

# project_warnings is omitted for the reason bench/CMakeLists.txt already records:
# the strict set (-Wold-style-cast, -Wconversion, -Wsign-conversion) fires all over
# the pybind11 headers, which cast through PyObject* and index with Py_ssize_t.
#
# project_options is omitted too. pybind11_add_module already applies hidden
# visibility and LTO; scikit-build-core builds Release, which supplies -O3
# -DNDEBUG; and -march=native on this one thin dispatch TU buys nothing, because
# all the hot code lives in me_core, which IS built with project_options.
target_link_libraries(_core PRIVATE
    me_core
    me_replay           # market data replay and validation
    me_ds_lock          # LockQueue
    me_event_handler    # Logger
    Threads::Threads
)

# scikit-build-core assembles the wheel from the install tree, so the extension
# has to land next to the pure-Python package.
install(TARGETS _core DESTINATION matching_engine)
