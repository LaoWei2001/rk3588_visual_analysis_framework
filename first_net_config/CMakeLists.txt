cmake_minimum_required(VERSION 3.16)

project(first_net_config VERSION 2.1 LANGUAGES C)

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
