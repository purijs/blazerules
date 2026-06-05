cmake_minimum_required(VERSION 3.22)
project(blazerules LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_OSX_ARCHITECTURES "arm64")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

message(STATUS "CMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}")
message(STATUS "CMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}")

add_compile_options(-Wall -Wextra -Wpedantic)
add_compile_options($<$<CONFIG:Debug>:-O0>)
add_compile_options($<$<CONFIG:Release>:-O3> $<$<CONFIG:Release>:-march=native>)

find_package(Arrow CONFIG REQUIRED)
find_package(simdjson CONFIG REQUIRED)
find_package(absl CONFIG REQUIRED)
find_package(TBB CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)

add_library(blazerules_core STATIC
        src/compiler/dsl_parser.cpp
)

add_executable(blazerules_driver src/driver/main.cpp)

target_include_directories(blazerules_core
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(blazerules_core
        PUBLIC
        Arrow::arrow_static
        PRIVATE
        simdjson::simdjson
        absl::flat_hash_map
        TBB::tbb
        yaml-cpp::yaml-cpp
)

target_link_libraries(blazerules_driver PRIVATE blazerules_core)