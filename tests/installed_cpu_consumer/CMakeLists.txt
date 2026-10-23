cmake_minimum_required(VERSION 3.25)

project(rexglue_installed_cpu_consumer LANGUAGES CXX)

find_package(rexglue 0.24.0 REQUIRED CONFIG)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(rexruntime_cpu_consumer cpu_consumer.cpp)
target_link_libraries(rexruntime_cpu_consumer PRIVATE rex::runtime)

add_executable(rexruntime_plugin_abi_check plugin_abi_check.cpp)
target_link_libraries(rexruntime_plugin_abi_check PRIVATE rex::runtime)

enable_testing()
add_test(NAME rexruntime_cpu_consumer COMMAND rexruntime_cpu_consumer)
