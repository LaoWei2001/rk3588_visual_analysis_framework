# 当前源码模块地图

下表对应 `vision_analysis/src/` 当前实际目录。

| 目录 | 当前责任 | 常用入口 |
|---|---|---|
| `capturer/` | GStreamer RTSP/file/USB 解码、共享源、bus 重连、appsink 回调 | `decChannel.h/.cpp` |
| `common/` | 线程安全日志等小型公共工具 | `logging.h` |
| `config/` | JSON 结构、注册字段、完整/热更验证 | `config.h/.cpp`、`config_validator.*` |
| `control/` | Unix socket Action 入队和系统级 `infer_toggle` | `logic_control.h/.cpp` |
| `display/` | 宫格合成、RGA 转换、系统/自定义叠加、FreeType 文本 | `display.h`、`display_pipeline.h` |
| `event/` | EventRequest、本地 schema v3、图片任务、投递初始状态 | `event_report.h/.cpp` |
| `gpio/` | libgpiod 引脚解析、输入输出 | `gpio.h/.cpp` |
| `inference/` | 模型实例、任务队列、RKNN worker、同帧结果发布与热换 | `inference_engine.h` |
| `logic/core/` | Channel/Global Context、注册表、参数、outputs | `channel_logic.h`、`global_logic.h` |
| `logic/modules/` | 单通道可插拔业务 | 每模块 `logic.cpp + logic.json` |
| `logic/global_modules/` | 全局可插拔业务 | 每模块 `logic.cpp + logic.json` |
| `pipeline/` | 帧入口、惰性转换、tracker 后业务调用、显示/结果分发 | `pipeline_runtime.h` |
| `recorder/` | 事件视频源帧环形缓冲、叠加和 MP4 编码 | `event_video_recorder.h/.cpp` |
| `rtsp/` | 拼接画面 RTSP 输出 | `rtsp_streamer.h/.cpp` |
| `runtime/` | APP_CTRL、不可变快照、在线状态、热重载、pause/constants | `app_ctrl.h/.cpp` |
| `third_party/gst_opt/` | 随引擎编译的 GStreamer 辅助实现 | `gst_opt.h` |
| `third_party/json/` | vendored cJSON | `cJSON.h/.c` |
| `tracking/` | 每通道 tracker | `tracker.h/.cpp` |
| `yolo/` | YOLOv5/seg/v8 det/pose 的模型封装和后处理 | `model_base.h`、`composite_model.h` |

顶层 `main.cpp` 只负责编排生命周期、信号和线程，不应吸收业务规则。

## 扩展放置原则

| 需求 | 放置位置 |
|---|---|
| 新单通道业务规则/参数/按钮 | 新 `logic/modules/<id>/` |
| 新跨通道组合规则 | 新 `logic/global_modules/<id>/` |
| 新通道输出数据类型 | 先评估 `logic_outputs` 公共契约和所有消费者 |
| 新模型后处理格式 | `yolo/` + inference 组合层 + 配置验证/Web 类型 |
| 新输入源 | `capturer/` + stream config + Web 双向转换 |
| 新显示叠加 primitive | `logic` API + `display` 渲染 + 图片/视频出口 |
| 新事件媒体 | `event` + `recorder`/生成 worker + Python contract/adapter + Web |
| 新远端协议 | `service/upload/adapters/` 和 catalog；不要放进 C++ logic |
| 新 Web 设备功能 | FastAPI router/service + React client/page + 权限/测试 |

## 构建收集与依赖

`CMakeLists.txt` 显式收集 config/runtime/capturer/pipeline/inference/tracking/control/gpio/display/rtsp/yolo/
recorder/event，并递归收集整个 `src/logic`。logic manifest 会在构建时验证并嵌入二进制。

当前必需库包括 OpenCV（必须有 `opencv2/freetype.hpp`）、GTK3、Threads、RGA、RKNN runtime 和
libgpiod，以及 GStreamer helper 所需库。找不到 OpenCV freetype 时 CMake 明确失败，不会回退到
Hershey 字体。

`build.sh` 在 aarch64/armv7l 使用板端原生构建，在其他架构使用 Docker 镜像
`rk3588_builder:2026_4_30`（可通过 `--image` 指定）。完整打包还会复制 assets、Python 服务、库，
生成 `logics.json`、`report_templates/` 和运行/环境初始化脚本。

## 已删除的历史边界

当前没有 `src/analyzer`、`src/core`、`src/player`，上传器也不在 C++ `src/uploader`。对应能力已经
分别落到 `pipeline/inference/runtime/display` 和仓库根 `service/upload`。任何设计或提示词如果还要求
修改这些旧目录，应先判为过期，而不是新建同名目录恢复旧架构。
