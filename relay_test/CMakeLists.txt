cmake_minimum_required(VERSION 3.10)

project(relay_test LANGUAGES C)

find_library(GPIOD_LIBRARY NAMES gpiod)
if(NOT GPIOD_LIBRARY)
    message(FATAL_ERROR "找不到 libgpiod")
endif()

add_executable(relay_test main.c)
target_compile_features(relay_test PRIVATE c_std_11)
target_compile_options(relay_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(relay_test PRIVATE ${GPIOD_LIBRARY})

set_target_properties(relay_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
)
