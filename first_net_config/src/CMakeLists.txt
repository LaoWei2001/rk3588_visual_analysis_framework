cmake_minimum_required(VERSION 3.16)

project(first_net_config VERSION 2.1 LANGUAGES C)

add_executable(first_net_config
    main.c
    cli_io.c
    command_runner.c
    device_name.c
    ipv4_utils.c
    nmcli_parser.c
    network_state.c
    network_health.c
    saved_connections.c
    network_safety.c
    network_workflows.c
    interface_inspector.c
    camera_discovery.c
    factory_restore.c
    network_cleanup.c
)

target_compile_features(first_net_config PRIVATE c_std_11)

target_compile_options(first_net_config PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

set_target_properties(first_net_config PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

include(CTest)
set(FIRST_NET_CONFIG_TEST_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_input.c")
if(BUILD_TESTING AND EXISTS "${FIRST_NET_CONFIG_TEST_SOURCE}")
    add_executable(first_net_config_tests
        "${FIRST_NET_CONFIG_TEST_SOURCE}"
        cli_io.c
        ipv4_utils.c
        nmcli_parser.c
    )
    target_include_directories(first_net_config_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_features(first_net_config_tests PRIVATE c_std_11)
    target_compile_options(first_net_config_tests PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    add_test(NAME first_net_config_input COMMAND first_net_config_tests)
elseif(BUILD_TESTING)
    message(STATUS "未找到 tests/test_input.c，跳过测试目标，不影响主程序构建")
endif()
