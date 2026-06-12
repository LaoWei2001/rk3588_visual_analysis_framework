# 配置模块 (config/)

> 负责从 JSON 文件解析运行时配置、验证合法性，并通过注册表机制支持字段级热重载。

---

## 文件清单

| 文件 | 职责 |
|------|------|
| `config.h` | 所有配置结构体定义（`AppConfig`、`ChannelConfig`、`StreamConfig` 等） |
| `config.cpp` | `load_config()` JSON 解析实现、`config_utils` 命名空间工具函数 |
| `config_init.cpp` | 注册表初始化：调用 `REG_G/REG_C` 宏注册所有字段 |
| `config_registry.h` | `ConfigRegistry` 类接口声明，定义 `REG_G/REG_C` 宏 |
| `config_registry.cpp` | `ConfigRegistry` 实现：字段解析、字段级同步（`sync_fields`） |
| `config_validator.h` | `validate_config()` 接口 |
| `config_validator.cpp` | 配置合法性校验逻辑 |

---

## 配置文件格式

配置文件为 JSON，`schema_version = 2`，启动时通过命令行参数指定路径（默认 `./assets/config.json`）。

### 顶层结构

```json
{
  "schema_version": 2,
  "global": { /* 全局参数 */ },
  "channels": [ /* 每通道参数列表 */ ],
  "global_logics": [ /* 全局逻辑实例列表（可选）*/ ]
}
```

### 全局参数（`global` 对象）

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `model_type` | string | `"yolo"` | 全局默认模型类型 |
| `model_path` | string | `""` | 全局默认 RKNN 模型文件路径 |
| `label_path` | string | `""` | 全局默认标签文件路径 |
| `enable_display` | bool | `true` | 是否打开 GTK 显示窗口 |
| `disp_width` | int | `1920` | 显示分辨率宽（自动对齐 4 的倍数） |
| `disp_height` | int | `1080` | 显示分辨率高（自动对齐 2 的倍数） |
| `tile_cols` | int | `2` | 显示网格列数 |
| `tile_rows` | int | `2` | 显示网格行数（为 0 时自动计算） |
| `performance_display` | bool | `true` | 性能统计总开关：终端打印 Feed 性能日志，并叠加画面 FPS（与 `show_fps` 共同决定 FPS 是否上屏） |
| `debug_display` | bool | `false` | 是否开启 `DBG_PRINT` 调试打印 |
| `enable_pause_key` | bool | `false` | 暂停键开关：选中显示窗口按空格暂停/继续（需同时开启 `enable_display`） |
| `enable_rtsp` | bool | `false` | 是否启用内置 RTSP 推流服务（无显示器时用 VLC / 配置平台看拼接画面） |
| `rtsp_port` | int | `8554` | RTSP 端口，地址 `rtsp://<板IP>:<port><rtsp_path>` |
| `rtsp_path` | string | `"/live"` | RTSP 挂载点（须以 `/` 开头） |
| `rtsp_fps` | int | `25` | 推流帧率 |
| `rtsp_bitrate` | int | `4096` | 软件编码码率(kbps)；硬件编码用默认码率 |
| `rtsp_codec` | string | `"h264"` | 推流编码 `"h264"` / `"h265"` |
| `rtsp_encoder` | string | `"auto"` | `"auto"`=有硬件就硬编否则软编，`"hw"`=强制硬编，`"sw"`=强制软编 |
| `channel_threads` | int | `1` | 每通道并发推理线程数（默认值） |
| `max_fps` | int | `30` | 每通道推理帧率上限（默认值） |
| `local_default_fps` | int | `25` | 本地文件播放时的采样帧率 |
| `queue_size` | int | `1` | 推理任务队列深度 |
| `npu_cores` | int | `3` | 使用的 RKNN 上下文数量（1~3） |
| `obj_thresh` | float | `0.4` | 目标置信度阈值（全局默认） |
| `nms_thresh` | float | `0.45` | NMS 重叠阈值（全局默认） |
| `detect_classes` | string[] | `[]` | 检测类别白名单，空数组=全部 |
| `tracker_enable` | int | `1` | 跟踪器开关（全局默认，0=关，1=开） |
| `tracker_iou_thresh` | float | `0.3` | 跟踪器 IoU 匹配阈值 |
| `tracker_max_miss` | int | `10` | 跟踪器最大连续丢失帧数 |
| `tracker_min_hits` | int | `3` | 轨迹确认所需最小命中帧数 |

### 通道参数（`channels` 数组元素）

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `id` | int | — | 通道唯一 ID（用于 OTA 匹配） |
| `enable` | bool | `true` | 是否启用该通道 |
| `infer_enable` | bool | `true` | 是否启用 YOLO 推理；`false`=传统算法通道（仍解码/显示/逐帧跑逻辑，不进 NPU，`ctx->results` 为空） |
| `stream.url` | string | — | RTSP 地址 |
| `stream.device` | string | — | USB 摄像头节点，如 `/dev/video0` |
| `stream.src_type` | string | **必填** | `"rtsp"` / `"file"` / `"usb"`（已取消自动推断，必须显式声明） |
| `stream.video_enc` | string | `"h264"` | 视频编码类型（`"h264"` 或 `"h265"`） |
| `stream.loop` | bool | `false` | 本地文件是否循环播放 |
| `stream.usb_width` | int | `0` | USB 显式采集宽（0=随 fps 自动档）；设定后与 ROI 抓帧坐标统一、不随 fps 变 |
| `stream.usb_height` | int | `0` | USB 显式采集高（0=随 fps 自动档） |
| `logic` | string | `"logic_default"` | 绑定的通道逻辑名称 |
| `model_type` | string | 继承全局 | 通道独立模型类型 |
| `model_path` | string | 继承全局 | 通道独立模型路径，空=不做推理 |
| `label_path` | string | 继承全局 | 通道独立标签路径 |
| `obj_thresh` | float | `-1`（继承全局）| 通道独立置信度阈值 |
| `nms_thresh` | float | `-1`（继承全局）| 通道独立 NMS 阈值 |
| `detect_classes` | string[] | 继承全局 | 通道独立类别白名单 |
| `threads` | int | `-1`（继承全局）| 通道独立并发推理线程数 |
| `playback_fps` | int | `-1` | 采集帧率上限（RTSP/文件/USB），`-1`=不限制 |
| `max_fps` | int | `-1`（继承全局）| 推理帧率上限 |
| `npu_core` | int | `-1`（自动分配）| 绑定 NPU 核心（0/1/2） |
| `tracker_enable` | int | `-1`（继承全局）| 通道独立跟踪器开关 |
| `tracker_iou_thresh` | float | 继承全局 | 通道独立跟踪器 IoU 阈值 |
| `tracker_max_miss` | int | 继承全局 | 通道独立最大丢失帧数 |
| `tracker_min_hits` | int | 继承全局 | 通道独立最小命中帧数 |
| `radius` | int | `1` | logic_hook 安全圈半径（自定义业务参数示例） |
| `report_interval_sec` | int | `5` | logic_server / logic_dify 两次上报的最小间隔（秒） |
| `dify_prompt` | string | `""` | logic_dify 提示词，空=用默认 |
| `server_url` | string | `""` | 该通道 HTTP 上报地址（方案2，空=用上报服务默认值） |
| `dify_api_url` / `dify_api_key` | string | `""` | 该通道 Dify 地址 / 密钥（方案2，空=用默认） |
| logic 专属算法参数 | — | — | 如 logic_wafer（`line_width`/`t_start`/`t_end`/`coverage_threshold`/`required_actions`）、logic_wafer_sop（`sop_sequence`/`basket_*`/`sop_*`）、logic_fall_detect（`fall_*`/`wave_*`）。完整清单与默认值见 `src/logic/logics.json` 与 `config.h` 的 `ChannelConfig`，此处不逐一列出 |

### 全局逻辑参数（`global_logics` 数组元素）

```json
{
  "enable": true,
  "logic": "global_example",
  "channels": [0, 1, 2],
  "poll_interval_ms": 100
}
```

| 字段 | 说明 |
|------|------|
| `enable` | 是否启动此实例 |
| `logic` | 全局逻辑函数名称（在 `global_logic.cpp` 中注册） |
| `channels` | 监控的通道号列表，空数组 `[]` = 监控所有通道 |
| `poll_interval_ms` | 轮询间隔（毫秒），实际值会自动收敛到推理帧间隔的一半 |

---

## 架构设计

### 配置注册表（`ConfigRegistry`）

所有字段在 `config_init.cpp` 中通过宏声明，系统在运行时按注册表解析 JSON 并执行字段级同步：

```
config_init.cpp
  REG_G("obj_thresh", FLOAT, obj_thresh)   → global_fields 列表
  REG_C("logic",      STRING, logic)        → channel_fields 列表

load_config()
  parse_global(json_obj, &cfg)             → 按 global_fields 批量赋值
  parse_channel(json_obj, &ch_cfg)         → 按 channel_fields 批量赋值

config_monitor_thread (热重载)
  sync_fields(&old_cfg, &new_cfg, true)    → 字段级原地同步（不析构 vector）
```

**为什么用字段级同步而不是整体赋值 `config = new_cfg`？**  
整体赋值会触发 `std::vector<ChannelConfig>` 的析构和重新分配，导致 `analyzer.cpp` 中持有的通道指针失效，产生 use-after-free。`sync_fields()` 通过偏移量原地更新每个字段，保证指针有效性。

### 继承机制（通道覆盖全局默认值）

通道配置中值为 `-1` / `""` / 空数组 的字段，在 `load_config()` 结束后会被赋予对应的全局默认值。核心逻辑在 `config.cpp` 中 `apply_channel_defaults()` 函数。

### 流类型（src_type 必填，不再自动推断）

`stream.src_type` 是**必填**字段，必须显式声明为 `"rtsp"` / `"file"` / `"usb"` 之一。
前后端均已**取消**按 `url`/`device` 自动推断类型的逻辑：

- C++ `config_utils::normalize_src_type(stream)` 仅做大小写归一；为空即视为配置错误，
  `load_config()` 与 `ConfigValidator` 都会拒绝。
- 前端（Web 控制台）视频流节点的「输入类型」必须手动选择；缺省会标红提示「未指定」。

---

## 热重载机制

修改配置文件后**无需重启程序**，`config_monitor_thread` 每 2 秒检测文件 `mtime`，变化后延迟一轮再重载（等待文件写入稳定），随后自动生效以下内容：

- 检测阈值（`obj_thresh`、`nms_thresh`）
- 检测类别白名单（`detect_classes`）
- 跟踪器参数
- 通道逻辑名称（`logic`）—— 切换时自动清空跨帧状态
- RKNN 模型（`model_path` / `model_type` 变化时热加载新模型）

**不支持热重载的字段**（需要重启生效）：通道数量、RTSP 地址、显示分辨率、NPU 核心数。

---

## 二次开发指南

### 添加新的全局配置字段

1. 在 `config.h` 的 `AppConfig` 结构体中声明字段：

```cpp
// config.h
struct AppConfig {
    // ... 已有字段 ...
    int my_new_param = 42;  // ← 新增
};
```

2. 在 `config_init.cpp` 中注册到解析表：

```cpp
// config_init.cpp
REG_G("my_new_param", INT, my_new_param);
```

3. 在 `config.json` 的 `global` 对象中添加对应键，重启或热重载即可生效。

### 添加新的通道配置字段

1. 在 `config.h` 的 `ChannelConfig` 中声明：

```cpp
struct ChannelConfig {
    // ...
    float alert_distance = 100.0f;  // ← 新增
};
```

2. 在 `config_init.cpp` 中注册：

```cpp
REG_C("alert_distance", FLOAT, alert_distance);
```

3. 在业务逻辑中通过 `ctx->config->alert_distance` 直接读取。

### 在业务逻辑中读取配置

通道配置通过 `ChannelContext` 传递，在逻辑函数中随时可读：

```cpp
static void logic_my(ChannelContext *ctx) {
    // 读取通道配置字段
    int r = ctx->config->radius;
    float thresh = ctx->config->obj_thresh;
    bool tracker_on = (ctx->config->tracker_enable == 1);
}
```

全局配置通过 `g_pCtrl->config` 读取（需持共享锁）：

```cpp
#include "core/app_ctrl.h"

int max_fps;
{
    std::shared_lock<std::shared_timed_mutex> lock(g_pCtrl->mtx);
    max_fps = g_pCtrl->config.max_fps;
}
```

---

## 配置示例

参见 `assets/config_file.json`（本地文件单路）、`assets/config_mux16.json`（16 路 RTSP）等示例文件。
