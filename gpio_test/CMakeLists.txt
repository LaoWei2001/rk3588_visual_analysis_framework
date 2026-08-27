cmake_minimum_required(VERSION 3.10)

project(gpio_test LANGUAGES C)

find_library(GPIOD_LIBRARY NAMES gpiod)
if(NOT GPIOD_LIBRARY)
    message(FATAL_ERROR "找不到 libgpiod")
endif()

add_executable(gpio_test main.c)
target_compile_features(gpio_test PRIVATE c_std_11)
target_compile_options(gpio_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(gpio_test PRIVATE ${GPIOD_LIBRARY})

set_target_properties(gpio_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
)
