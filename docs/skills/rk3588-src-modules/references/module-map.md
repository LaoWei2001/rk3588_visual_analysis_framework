# 当前源码模块地图

下表目录默认相对 `engine/src/`；`engine/vendor/` 和 `projects/` 使用仓库根相对路径。
完整目录和构建导航见 [engine/README.md](../../../../engine/README.md)。

| 目录 | 当前责任 | 常用入口 |
|---|---|---|
| `media/capture/` | GStreamer RTSP/file/USB 解码、共享源、bus 重连、appsink 回调 | `decode_channel.h/.cpp` |
| `common/` | 线程安全日志等小型公共工具 | `logging.h` |
| `application/config/` | JSON 结构、注册字段、完整/热更验证 | `config.h/.cpp`、`config_validator.*` |
| `application/control/` | Unix socket 动作、播放控制、数据采集 | `logic_control.*`、`file_playback_control.*`、`remote_dataset.*` |
| `media/display/` | 宫格合成、RGA 转换、系统/自定义叠加、FreeType 文本 | `display.h`、`display_render.h` |
| `events/` | 本地 schema v3、图片任务、持久化、投递初始状态 | `event_report.cpp`、`event_storage.h`；公开 `engine/include/events.h` |
| `dataset/` | 数据集采集规则求值 | `rules.h/.cpp` |
| `platform/gpio/` | libgpiod 引脚解析、输入输出 | `gpio.cpp`、`gpio_runtime.h`；公开 `engine/include/gpio.h` |
| `vision/inference/` | 模型实例、任务队列、RKNN worker、同帧结果发布与热换 | `inference_engine.h` |
| `logic/` | Channel/Global Context、注册表、参数、outputs | `channel_context.cpp`、`target_query.cpp`、`global_logic.cpp` |
| `projects/modules/` | 单通道可插拔业务 | 每模块 `logic.cpp + logic.json` |
| `projects/global_modules/` | 全局可插拔业务 | 每模块 `logic.cpp + logic.json` |
| `pipeline/` | 帧入口、惰性转换、tracker 后业务调用、显示队列消费与结果分发 | `pipeline_runtime.h`、`display_worker.h` |
| `media/recorder/` | 事件视频源帧环形缓冲、叠加和 MP4 编码 | `event_video_recorder.h/.cpp` |
| `media/rtsp/` | 拼接画面 RTSP 输出 | `rtsp_streamer.h/.cpp` |
| `application/runtime/` | APP_CTRL、不可变快照、在线状态、热重载、pause/constants | `app_ctrl.h/.cpp` |
| `engine/vendor/gst_opt/` | 随引擎编译的 GStreamer 辅助实现 | `gst_opt.h` |
| `engine/vendor/cjson/` | vendored cJSON | `cJSON.c`、公开 `engine/include/json.h` |
| `vision/tracking/` | 每通道 tracker | `tracker.h/.cpp` |
| `vision/models/` | 模型公共接口、模型组装与结果合并、RKNN 元数据检查与输出缓冲、结果几何 | `model_base.h`、`model_group.h`、`model_inspection.h` |
| `vision/models/yolo/` | YOLO 检测、分割、姿态、深度的模型封装和后处理 | `yolo.h`、`yolov8det.h`、`yolo26pose.h` |
| `vision/models/zip-depth/` | ZipDepth 相对逆深度模型适配 | `zip_depth.h/.cpp` |

`application/main.cpp` 只负责编排生命周期、信号和线程，不应吸收业务规则。

## 扩展放置原则

| 需求 | 放置位置 |
|---|---|
| 新单通道业务规则/参数/按钮 | 新 `projects/modules/<id>/` |
| 新跨通道组合规则 | 新 `projects/global_modules/<id>/` |
| 新通道输出数据类型 | 先评估 `logic_outputs` 公共契约和所有消费者 |
| 新模型后处理格式 | `vision/models/<适配器>/` + inference 工厂 + 配置验证/Web 类型 |
| 新输入源 | `media/capture/` + stream config + Web 双向转换 |
| 新显示叠加 primitive | `logic` API + `media/display` 渲染 + 图片/视频出口 |
| 新事件媒体 | `events` + `media/recorder`/生成 worker + Python contract/adapter + Web |
| 新远端协议 | `services/framework/upload/adapters/` 和 catalog；不要放进 C++ logic |
| 新 Web 设备功能 | FastAPI router/service + React client/page + 权限/测试 |

## 构建收集与依赖

`engine/CMakeLists.txt` 加载 `cmake/` 下的工具链、依赖、业务、主程序和测试定义。
`cmake/Engine.cmake` 显式登记框架实现文件；新增 `.cpp` 时必须更新该列表。
`cmake/Business.cmake` 递归收集 `projects/modules` 与 `projects/global_modules`；
新增业务目录后应重新运行 CMake。Logic manifest 会在构建时验证并嵌入二进制。

当前必需库包括 OpenCV（必须有 `opencv2/freetype.hpp`）、GTK3、Threads、RGA、RKNN runtime 和
libgpiod，以及 GStreamer helper 所需库。找不到 OpenCV freetype 时 CMake 明确失败，不会回退到
Hershey 字体。

`build.sh` 在 aarch64/armv7l 使用板端原生构建，在其他架构使用 Docker 镜像
`rk3588_builder:2026_4_30`（可通过 `--image` 指定）。完整打包还会复制 assets、Python 服务、库，
生成 `logics.json`、`report_templates/` 和运行/环境初始化脚本。

## 已删除的历史边界

当前没有 `src/analyzer`、`src/core`、`src/player`，上传器也不在 C++ `src/uploader`。对应能力已经
分别落到 `pipeline/`、`vision/`、`application/runtime/` 、`media/` 和仓库根 `services/framework/upload`。任何设计或提示词如果还要求
修改这些旧目录，应先判为过期，而不是新建同名目录恢复旧架构。
