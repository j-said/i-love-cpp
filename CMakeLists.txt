cmake_minimum_required(VERSION 3.20)
project(packet_pool CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(packet_pool INTERFACE)
target_include_directories(packet_pool INTERFACE include)

add_executable(benchmark src/benchmark.cpp)
target_link_libraries(benchmark PRIVATE packet_pool)
target_compile_options(benchmark PRIVATE -O2 -pthread)
target_link_options(benchmark PRIVATE -pthread)

add_executable(server_pool src/server_pool.cpp)
target_link_libraries(server_pool PRIVATE packet_pool)
target_compile_options(server_pool PRIVATE -O2 -pthread)
target_link_options(server_pool PRIVATE -pthread)

add_executable(stress src/stress.cpp)
target_compile_options(stress PRIVATE -O2 -pthread)
target_link_options(stress PRIVATE -pthread)

add_executable(benchmark_multi_arena src/benchmark_multi_arena.cpp)
target_link_libraries(benchmark_multi_arena PRIVATE packet_pool)
target_compile_options(benchmark_multi_arena PRIVATE -O2 -pthread)
target_link_options(benchmark_multi_arena PRIVATE -pthread)
