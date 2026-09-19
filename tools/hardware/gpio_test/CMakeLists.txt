cmake_minimum_required(VERSION 3.10)

project(rk3588_gpioctl LANGUAGES C)

find_library(GPIOD_LIBRARY NAMES gpiod)
if(NOT GPIOD_LIBRARY)
    message(FATAL_ERROR "找不到 libgpiod")
endif()

add_executable(rk3588-gpioctl main.c)
target_compile_features(rk3588-gpioctl PRIVATE c_std_11)
target_compile_options(rk3588-gpioctl PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(rk3588-gpioctl PRIVATE ${GPIOD_LIBRARY})

set_target_properties(rk3588-gpioctl PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
)

install(TARGETS rk3588-gpioctl RUNTIME DESTINATION bin)
