---
name: rk3588-src-modules
description: >-
  Explain, review, or extend the current C++ engine architecture and runtime
  configuration of this RK3588 visual-analysis project. Use for source-module
  ownership, frame and inference pipelines, threading, immutable snapshots,
  configuration parsing or hot reload, build dependencies, startup, shutdown,
  and deciding where an engine-level change belongs. Verify every public claim
  against current headers and implementation before changing shared APIs.
---

# RK3588 引擎源码与配置

本 Skill 是框架层二次开发入口。它替代过去按旧 `analyzer/core/player/uploader` 目录拆分的说明；这些
目录在当前 `vision_analysis/src/` 中不存在，不得继续作为设计边界。

## 当前权威入口

- 生命周期：`vision_analysis/src/main.cpp`；
- 配置：`src/config/config.h/.cpp`、`config_init.cpp`、`config_validator.cpp`；
- 运行状态与热更：`src/runtime/app_ctrl.h/.cpp`；
- 帧管线：`src/pipeline/`；
- 推理：`src/inference/` 和 `src/yolo/`；
- 业务扩展：`src/logic/`；
- 构建：`vision_analysis/CMakeLists.txt`、`build.sh`。

## 修改前的架构判断

1. 需求能否只在现有通道/全局 logic 完成？能则不要扩展中央结构。
2. 是公开业务契约还是底层机制？业务参数进入模块 `logic.json`，不进入 `ChannelConfig`。
3. 是否改变线程/通道/显示/RTSP 拓扑？这类变化通常要求重启，不能伪装成普通热更。
4. 是否需要像素？沿用 `LazyVideoFrame` 的惰性模型帧/源帧，不在无消费者时提前转换。
5. 是否跨线程共享？优先不可变 `shared_ptr` 快照和既有 mutex 所有权，不暴露可变裸状态。
6. 是否会影响 catalog、Web、上传模板或 OTA？同时核对对应生成器和消费方。

## 运行数据流

```text
GStreamer capturer/appsink
  → pipeline_submit_frame
      ├─ 显示/RTSP 最新帧队列
      ├─ 非推理通道：按 max_fps → channel logic（空 results）
      ├─ 推理通道：TaskQueue → NPU worker → result dispatch
      │                              → tracker → channel logic
      └─ 事件视频源帧环形缓冲（仅配置需要视频时）

channel publication（frame/results/outputs/draw/state 元信息）
  → global logic 各实例独立轮询
  → report_event → 本地异步事件/媒体 → Python 上传服务
```

每通道 `g_process_mtx[id]` 串行化推理结果与非推理直通两条业务路径。Channel logic 在
`chn_mtx[id]` 外执行，返回后短锁提交；frame/results/outputs/draws 和发布元信息形成同一版本。

## 按任务加载参考页

- 生命周期、线程、数据流、锁和快照：
  [引擎架构](references/architecture.md)
- JSON 结构、验证、废弃字段与热重载：
  [配置参考](references/configuration.md)
- 当前源码目录、职责、依赖和扩展落点：
  [模块地图](references/module-map.md)
- 通道公开业务 API：
  [`rk3588-channel-logic`](../rk3588-channel-logic/SKILL.md)
- 全局公开业务 API：
  [`rk3588-global-logic`](../rk3588-global-logic/SKILL.md)

## 兼容性规则

- `config.channels[].id` 是通道唯一身份，不是配置数组下标；显示顺序另算。
- 当前模型配置唯一入口是每通道 `models[]`，一个通道可有多个模型。
- ROI 配置为归一化 `roi_zones[]`，运行快照发布时转换到模型坐标。
- 无 logic 仍提交帧/结果；无推理但有 logic 时以空 results 调用。
- 当前配置快照整代不可变，callback 期间不能观察到半更新值。
- 新增 logic 目录由 CMake 递归收集，但需要重新运行 CMake/构建并生成 catalog。
- 不直接编辑打包输出中的 `logics.json` 或 `report_templates/` 代替源码修改。

## 验证

无需二进制的静态检查：

```bash
cd vision_analysis
python3 scripts/generate_logics_catalog.py --check
```

仓库当前没有提交预编译二进制；仅在已经用当前源码得到 `./vision_analysis` 后再做运行时能力探测：

```bash
./vision_analysis --list-logics
./vision_analysis --list-global-logics
./vision_analysis --validate-config ./assets/config_6.json
```

`config_6.json` 只是仓库现存示例；验收具体应用时应换成实际运行配置。

编译验证：

```bash
./build.sh --debug
```

完整交付再运行 `./build.sh my_app`（把 `my_app` 换成单层输出目录名）。公共接口、线程或热重载变更还必须做板端真实流、
断流重连、多通道、退出和失败回滚测试。
