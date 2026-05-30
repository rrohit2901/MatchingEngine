cmake_minimum_required(VERSION 3.20)  
project(matching_engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(me_core src/order_book.cpp src/matching.cpp)
target_include_directories(me_core PUBLIC include)
target_compile_options(me_core PRIVATE -O3 -march=native -Wall -Wextra -Wpedantic)

add_executable(me_main src/main.cpp)
target_link_libraries(me_main PRIVATE me_core)
