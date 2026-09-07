cmake_minimum_required(VERSION 3.16)

project(first_net_config VERSION 2.1 LANGUAGES C)

set(CURSES_NEED_WIDE TRUE)
find_package(Curses REQUIRED)

add_executable(first_net_config
    main.c
    common/cli_io.c                        # 基础公共组件
    common/command_runner.c                # 基础公共组件
    common/terminal_ui.c                   # ncursesw 终端界面
    common/intro_animation.c               # 独立三维启动动画
    common/anonymous_intro.c               # 独立匿名者字符启动页
    common/cube_intro.c                    # 独立三维立方体启动动画
    common/intro_notice.c                  # 启动页版权与许可证声明
    common/startup_intro.c                 # 启动页随机选择器
    operations/device_name.c               # 配置操作工作流
    common/ipv4_utils.c                    # 基础公共组件
    common/nmcli_parser.c                  # 基础公共组件
    common/wifi_signal_parser.c            # 基础公共组件
    inspection/network_quality_monitor.c   # 探测、状态与监测
    inspection/network_state.c             # 探测、状态与监测
    inspection/network_health.c            # 探测、状态与监测
    operations/saved_connections.c         # 配置操作工作流
    operations/network_safety.c            # 配置操作工作流
    operations/network_workflows.c         # 配置操作工作流
    inspection/interface_inspector.c       # 探测、状态与监测
    inspection/camera_discovery.c          # 探测、状态与监测
    operations/factory_restore.c           # 配置操作工作流
    operations/network_cleanup.c           # 配置操作工作流
)

target_include_directories(first_net_config PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CURSES_INCLUDE_DIRS}
)

target_link_libraries(first_net_config PRIVATE ${CURSES_LIBRARIES} m)

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
