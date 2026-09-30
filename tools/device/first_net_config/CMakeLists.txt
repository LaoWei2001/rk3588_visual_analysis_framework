cmake_minimum_required(VERSION 3.16)

project(first_net_config VERSION 2.2 LANGUAGES C)

add_executable(first_net_config
    src/main.c
    src/common/cli_io.c                        # 基础公共组件
    src/common/command_runner.c                # 基础公共组件
    src/common/terminal_ui.c                   # ANSI + termios 终端界面
    src/common/ansi_screen.c                   # 启动动画 ANSI 画布兼容层
    src/common/intro_animation.c               # 三维旋转面具启动动画
    src/common/anonymous_intro.c               # 匿名者字符显影启动动画
    src/common/cube_intro.c                    # 三维旋转立方体启动动画
    src/common/intro_notice.c                  # 启动页版权与许可证声明
    src/common/startup_intro.c                 # 三种 ANSI 启动动画
    src/operations/device_name.c               # 配置操作工作流
    src/operations/device_name_store.c         # 主机名持久化与 hosts 同步
    src/operations/system_access.c             # SSH 与系统用户管理
    src/common/ipv4_utils.c                    # 基础公共组件
    src/common/nmcli_parser.c                  # 基础公共组件
    src/common/wifi_signal_parser.c            # 基础公共组件
    src/inspection/network_quality_monitor.c   # 探测、状态与监测
    src/inspection/network_state.c             # 探测、状态与监测
    src/inspection/network_health.c            # 探测、状态与监测
    src/operations/saved_connections.c         # 配置操作工作流
    src/operations/network_safety.c            # 配置操作工作流
    src/operations/network_workflows.c         # 配置操作工作流
    src/inspection/interface_inspector.c       # 探测、状态与监测
    src/inspection/camera_discovery.c          # 探测、状态与监测
    src/operations/factory_restore.c           # 配置操作工作流
    src/operations/network_cleanup.c           # 配置操作工作流
)

target_include_directories(first_net_config PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(first_net_config PRIVATE m)

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
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

include(CTest)
if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_device_name_store.c")
    add_executable(test_device_name_store
        tests/test_device_name_store.c
        src/operations/device_name_store.c
    )
    target_include_directories(test_device_name_store PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_features(test_device_name_store PRIVATE c_std_11)
    target_compile_options(test_device_name_store PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    add_test(NAME device_name_store COMMAND test_device_name_store)
elseif(BUILD_TESTING)
    message(WARNING
        "BUILD_TESTING 已开启，但 tests/test_device_name_store.c 不存在；"
        "跳过 test_device_name_store，主程序仍可正常构建。"
    )
endif()
