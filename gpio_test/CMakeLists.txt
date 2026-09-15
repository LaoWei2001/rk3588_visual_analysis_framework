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

# gpio_test 是历史开发入口；主程序名称改为 rk3588-gpioctl，构建目录保留
# gpio_test 兼容链接，现有板端测试命令无需立即修改。
add_custom_command(TARGET rk3588-gpioctl POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E remove -f
            "$<TARGET_FILE_DIR:rk3588-gpioctl>/gpio_test"
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            "$<TARGET_FILE_NAME:rk3588-gpioctl>"
            "$<TARGET_FILE_DIR:rk3588-gpioctl>/gpio_test"
)

install(TARGETS rk3588-gpioctl RUNTIME DESTINATION bin)
