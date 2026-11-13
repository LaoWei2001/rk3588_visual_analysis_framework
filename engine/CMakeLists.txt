cmake_minimum_required(VERSION 3.10.2)

# Build with: cmake -S engine -B build/engine
# Cross compilation must select the toolchain before project().
include(cmake/Toolchain.cmake)
project(vision_analysis LANGUAGES C CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-Wall -Wpointer-arith -O3)

include(cmake/Dependencies.cmake)
include(cmake/Business.cmake)
include(cmake/Engine.cmake)
include(cmake/Tests.cmake)
