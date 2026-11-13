# C++ 引擎源码导航

本目录包含框架实现、公共业务 SDK 和锁定版本的第三方依赖。
应用业务放在仓库根目录的 `projects/modules/` 与 `projects/global_modules/`。

## 目录与职责

```text
engine/
├── include/                 公共 SDK；供业务模块使用
├── src/
│   ├── application/
│   │   ├── main.cpp         启动、主循环、停止顺序与退出结果
│   │   ├── config/          配置解析、字段注册和校验
│   │   ├── control/         动作、文件播放和数据采集控制接口
│   │   └── runtime/         运行状态、快照、信号、看门狗和退出确认
│   ├── pipeline/            帧入口、通道生命周期与结果分发
│   ├── media/
│   │   ├── capture/         GStreamer 输入、共享解码与重连
│   │   ├── display/         叠加、拼接和本地显示
│   │   ├── recorder/        事件视频缓存与编码
│   │   └── rtsp/            实时预览推流
│   ├── vision/
│   │   ├── inference/       任务队列、模型热换与 NPU 调度
│   │   ├── models/          YOLO、ZipDepth 模型适配和后处理
│   │   └── tracking/        SORT、ByteTrack
│   ├── events/              事件队列、媒体生成与持久化
│   ├── dataset/             数据集采集规则
│   ├── logic/               通道/全局业务上下文、参数与注册
│   ├── platform/gpio/       GPIO 实现与生命周期
│   └── common/              日志、时间和性能计数等内部工具
├── vendor/                  RKNN、Rockchip 平台清单、cJSON、GStreamer helper
├── cmake/                   按职责拆分的构建定义
├── tests/                   已有软件回归、硬件验证和性能工具
└── CMakeLists.txt           构建入口
```

`src/` 中使用相对该目录的完整 include，例如
`#include "application/runtime/app_ctrl.h"`、`#include "vision/inference/inference_engine.h"`。
公开 SDK 保持在 `include/`；业务模块通过 `Vision::SDK` 获取公开头文件和 OpenCV，
不应包含 `src/` 私有头文件或自行调用 RKNN。

这些目录按职责组织；引擎内部仍链接为一个可执行程序，并非相互隔离的独立库。
完整数据流、线程和快照约束见[模块地图](../docs/skills/rk3588-src-modules/references/module-map.md)
与[架构说明](../docs/skills/rk3588-src-modules/references/architecture.md)。

## 从哪里开始读

| 任务 | 入口 |
| --- | --- |
| 启动和安全退出 | [`application/main.cpp`](src/application/main.cpp)、[`runtime/shutdown_status.h`](src/application/runtime/shutdown_status.h) |
| 修改配置或热重载 | [`application/config/`](src/application/config)、[`runtime/app_ctrl.cpp`](src/application/runtime/app_ctrl.cpp) |
| 分析每帧处理流程 | [`pipeline/frame_inlet.cpp`](src/pipeline/frame_inlet.cpp)、[`pipeline/result_dispatch.cpp`](src/pipeline/result_dispatch.cpp) |
| 模型与 NPU 调度 | [`vision/inference/`](src/vision/inference)、[`vision/models/`](src/vision/models) |
| 事件落盘与媒体 | [`events/event_report.cpp`](src/events/event_report.cpp)、[`events/event_storage.h`](src/events/event_storage.h) |
| 新增业务规则 | [公开 SDK](include/README.md)、[业务示例](../projects/README.md) |

Web 的停止请求及进程结果判定在
[`process_manager.py`](../services/framework/runtime_manager/python/rk3588_runtime/process_manager.py)。
修改停止流程时应同时检查该文件和 C++ 退出状态写入，避免两端对“已退出”和“完整清理”的判定不一致。

## 构建定义

| 文件 | 职责 |
| --- | --- |
| [`cmake/Toolchain.cmake`](cmake/Toolchain.cmake) | 在 `project()` 前选择工具链 |
| [`cmake/Dependencies.cmake`](cmake/Dependencies.cmake) | 锁定 RKNN 并查找平台依赖 |
| [`cmake/Business.cmake`](cmake/Business.cmake) | 公开 SDK、业务源码和嵌入的 Logic 清单 |
| [`cmake/Engine.cmake`](cmake/Engine.cmake) | 显式登记框架实现文件、链接主程序 |
| [`cmake/Tests.cmake`](cmake/Tests.cmake) | 可选测试目标和 CTest 注册 |

新增框架 `.cpp` 文件后，更新 `cmake/Engine.cmake` 的源码列表。
新增业务模块后，重新运行 CMake；业务目录会递归收集，`logic.json` 会校验并嵌入程序。

从仓库根目录执行板端编译：

```bash
cmake -S engine -B build/engine
cmake --build build/engine -j4
```

需要匹配的 RK3588 BSP、平台库和开发依赖；详细条件见[快速开始](../README.md#快速开始)。
打包入口仍是根目录 `build.sh`。回归入口及清理命令见[贡献指南](../CONTRIBUTING.md)。
