# rk3588_yolo 源码模块说明（src/ 蒸馏）

> 本目录是 `rk3588_yolo/src/` 各模块的深度说明，按模块一份 `.md`。原先散落在 `src/<模块>/README.md`，
> 现统一收敛到这里，作为整个 C++ 端的“蒸馏文档”——既给后续开发者看，也可直接喂给大模型做二次开发提示词。
>
> 面向“任务”的指南（怎么写一个通道逻辑、全局逻辑、网页控制台运维）见同级的
> `../rk3588-channel-logic/`、`../rk3588-global-logic/`、`../rk3588-console-ops/`。

---

## 模块地图

| 模块 | 一句话职责 | 文档 |
|---|---|---|
| **core** | 全局控制块 `g_pCtrl`、通道状态与锁约定、配置热监控线程、跨通道安全取数 | [core.md](core.md) |
| **config** | `config.json` 解析/校验、注册表字段级热重载、通道继承全局默认值 | [config.md](config.md) |
| **capturer** | GStreamer 多路采集（RTSP/USB/文件）、硬解、帧率限流、阶梯退避重连 | [capturer.md](capturer.md) |
| **analyzer** | 帧处理总枢纽：FPS 节流 → RGA 转 640 → NPU 推理队列 → tracker → 调通道逻辑 → 分发显示 | [analyzer.md](analyzer.md) |
| **yolo** | RKNN 推理引擎：`ModelBase` 抽象 + YOLOv5/v8-det/v8-pose/v5-seg 实现 | [yolo.md](yolo.md) |
| **logic** | 通道业务逻辑框架：`ChannelContext`、`draw_*`、自注册分发表、各 `logic_*.cpp` | [logic.md](logic.md) |
| **player** | GTK/framebuffer 多路拼接显示 + `render_overlays` 叠加 + 内置 RTSP 推流 | [player.md](player.md) |
| **uploader** | 异步告警上报：编码 → 写 Redis 队列 → Python 微服务消费（HTTP / Dify） | [uploader.md](uploader.md) |

> 全局逻辑（跨通道、周期性）的框架在 `src/logic/global_logic.*`，任务指南见 `../rk3588-global-logic/`。

---

## 端到端数据流

```
[capturer] decChannel(GStreamer 硬解)
      │  解码出 NV12 帧
      ▼
[analyzer] videoOutHandle()  ← 帧处理总入口(analyzer.cpp)
      │   ├─ FPS 节流(frame_inlet): 未到推理时间 → 只送显示, 跳过 NPU
      │   ├─ RGA 转换(rga_convert): 源帧 → 模型输入 640×640 BGR(整幅拉伸, 无 letterbox)
      │   ├─ 送推理队列(algoProcess) ──► [yolo] NPU 推理(algo_engine: create_model) → vector<AlgoResult>
      │   └─ 异步送显示队列
      ▼
[analyzer] result_dispatch → channel_pipeline
      │   ├─ tracker(SORT): 填 AlgoResult.track_id
      │   └─ 构造 ChannelContext, 调用通道 logic
      ▼
[logic] logic_xxx(ctx)  ← 你的业务代码
      │   ├─ 读 ctx->results / ctx->rois / ctx->config / ctx->unix_ms ...
      │   ├─ draw_*(ctx, ...) 产出绘制指令(模型 640 坐标系)
      │   └─ (可选) alarm_uploader_enqueue(...) 触发上报
      ▼
[player] display_pipeline → display_render → render_overlays(display.cpp)
      │   RGA 缩放到 tile + 叠加框/ROI/文字/draw_cmds(坐标与线宽按 输出/640 等比缩放) → framebuffer / RTSP
      ▼
[uploader] 上传线程: server告警→落盘发件箱(alarm_store/) / dify→Redis(dify_queue) → Python 微服务 → HTTP/Dify
```

横切关注点：**core** 提供 `g_pCtrl` 全局状态与跨通道安全取数；**config** 的热监控线程在运行中改阈值/类别/logic/模型而不重启。

---

## 必须先懂的几个约定（贯穿全代码）

- **坐标系**：检测框、ROI、`draw_*`、`ctx->frame` 一律用**模型输入 640×640** 坐标系。预处理是**整幅拉伸**（非 letterbox 补边），渲染层按 `输出尺寸/640` 把坐标**和线宽/字号一起**等比缩放，所以 overlay 与画面/ROI 永远对齐、且随窗口等比缩放（见 player.md / `display.cpp`）。
- **线程与锁**：每通道一把 `g_pCtrl->chn_mtx[chnId]`；`ChannelState` 字段按“谁拥有”分三组，越组访问要持对应锁（见 core.md 的表）。跨通道取数只用 `ctx->get_channel_snapshot(ch)` / `app_ctrl_*`，它在一把锁内原子读出 frame+results+state，保证“同帧”。
- **时间**：`ctx->unix_ms`（Unix epoch 毫秒，墙钟，三种源统一）/ `ctx->time_hms()` / `ctx->datetime()` 是真实日历时间；`ctx->timestamp_ms` 是单调钟，只能算间隔。
- **颜色 BGR**：OpenCV 顺序 `cv::Scalar(B,G,R)`，不是 RGB。
- **自注册**：每个 `logic_xxx.cpp` 末尾 `REGISTER_LOGIC("logic_xxx", logic_xxx)` 在 `main()` 前登记；新增=加文件、删除=删文件，不动框架（见 logic.md）。

---

## 二次开发：从“我想做 X”到“改哪里”

| 我想… | 改哪个模块 / 看哪份文档 |
|---|---|
| 写一个通道业务逻辑 | [logic.md](logic.md) + `../rk3588-channel-logic/`（任务指南、API、示例） |
| 写跨通道/周期逻辑 | `src/logic/global_logic.*` + `../rk3588-global-logic/` |
| 接入新模型类型 | [yolo.md](yolo.md)（继承 `ModelBase` + `algo_engine.cpp` 的 `create_model`）+ config_validator |
| 加一个配置字段 | [config.md](config.md)（`config.h` 声明 + `config_init.cpp` 的 `REG_G/REG_C`） |
| 加一种视频源 | [capturer.md](capturer.md)（仿 `createUsbDecChannel`）+ config 的 `is_supported_src_type` |
| 自定义画面叠加 | [player.md](player.md) + logic 里用 `draw_*` |
| 触发告警上报 | [uploader.md](uploader.md)（`alarm_uploader_enqueue` / `dify_uploader_enqueue`） |
| 跨通道安全读另一路数据 | [core.md](core.md)（`app_ctrl_*` / `ctx->get_channel_snapshot`） |

---

## 构建与运行（速查）

```bash
# 板端原生编译(aarch64) 或 x86_64 Docker 交叉编译, 自动识别
./build.sh dist            # 编译+打包到 ./dist
./build.sh --debug         # 只编译可执行文件(Debug), 不打包
./rk3588_yolo ./assets/config_xxx.json   # 运行
```

> `src/logic` 下的 `.cpp` 由 CMake `aux_source_directory` 自动收集，新增/删除逻辑文件无需改 CMake。
