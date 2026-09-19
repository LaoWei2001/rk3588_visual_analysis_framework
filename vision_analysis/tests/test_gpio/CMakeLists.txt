cmake_minimum_required(VERSION 3.10)

project(gpio_api_test LANGUAGES CXX)

find_package(Threads REQUIRED)
find_library(GPIOD_LIBRARY NAMES gpiod)
if(NOT GPIOD_LIBRARY)
    message(FATAL_ERROR "找不到 libgpiod，请先安装 libgpiod 开发包")
endif()

add_executable(gpio_api_test
    main.cpp
    ../../src/gpio/gpio.cpp
)

target_compile_features(gpio_api_test PRIVATE cxx_std_14)
target_compile_options(gpio_api_test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
target_include_directories(gpio_api_test PRIVATE
    ../../src
    ../../include
)
target_link_libraries(gpio_api_test PRIVATE
    ${GPIOD_LIBRARY}
    Threads::Threads
)
