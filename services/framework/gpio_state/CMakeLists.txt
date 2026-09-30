cmake_minimum_required(VERSION 3.10)
project(rk3588_gpio_daemon LANGUAGES C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GPIOD REQUIRED IMPORTED_TARGET libgpiod)

add_executable(rk3588-gpio-daemon gpio_control_daemon.c)
target_compile_features(rk3588-gpio-daemon PRIVATE c_std_11)
target_compile_options(rk3588-gpio-daemon PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(rk3588-gpio-daemon PRIVATE PkgConfig::GPIOD)
