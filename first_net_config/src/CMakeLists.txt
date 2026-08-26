cmake_minimum_required(VERSION 3.16)

project(first_net_config VERSION 1.6 LANGUAGES C)

add_executable(first_net_config
    main.c
    cli_io.c
    command_runner.c
    ipv4_utils.c
    network_state.c
    network_safety.c
    network_workflows.c
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
