# 采集模块 (capturer/)

> 基于 GStreamer 的多路视频采集层，支持 RTSP 网络流、本地视频文件、USB 摄像头三种输入源，并实现自动重连与帧率限流。

---

## 文件清单

| 文件 | 职责 |
|------|------|
| `decChannel.h` | `DecChannel` 类声明、`SrcCfg_t` 输入源配置结构、`GstChannel_t` 管道元素结构 |
| `decChannel.cpp` | GStreamer 管道创建、帧回调、Bus 监听线程、重连逻辑 |

---

## 架构设计

每个通道对应一个 `DecChannel` 实例，各自维护一条独立的 GStreamer 管道。管道结构因输入源类型不同而有所差异：

### RTSP 管道

```
rtspsrc → rtph264depay/rtph265depay → h264parse/h265parse → mppvideodec → appsink
```

- `rtspsrc`：建立 RTSP 连接，协议强制 TCP（`protocols=0x04`，避免 UDP 丢包）
- `mppvideodec`：瑞芯微 MPP 硬件解码器，输出 NV12 格式
- `appsink`：`max-buffers=2, drop=TRUE`，丢弃积压旧帧，始终处理最新帧

### 本地文件管道

```
filesrc → decodebin → appsink
```

- `decodebin` 自动探测格式并构建解码链（内部为 `qtdemux → h264parse → mppvideodec`）
- 先切 PAUSED 完成 typefinding，再切 PLAYING，避免 decodebin 链路未完成时丢帧

### USB 摄像头管道

```
v4l2src → videoconvert → capsfilter(NV12) → appsink
```

- 自动按帧率映射合适的分辨率档位（15fps → 1280×720，30fps → 640×480 等）
- 提供后备 caps，协商失败时回退到设备默认格式

---

## 线程模型

每个 `DecChannel` 实例独占一个 **Bus 监听线程**（`busListen`），与 GStreamer 的 streaming 线程分离：

```
GStreamer streaming 线程（GLib 内部）
  │
  └─ new_sample 回调（appsink，帧到来时调用）
       ├─ 帧率限流：未到处理时间 → 丢弃，立即返回（不阻塞！）
       └─ videoOutHandle() → 送帧到 analyzer

Bus 监听线程（busListen，每通道一个 pthread）
  │
  └─ 轮询 GstBus（500ms 超时）
       ├─ GST_MESSAGE_ERROR  → 触发重连
       ├─ GST_MESSAGE_EOS    → RTSP/文件结束 → 触发重连或退出
       ├─ 静默断流（无新帧超时）:
       │    文件流 3s、实时流 15s 无新帧 → 触发重连
       └─ 退出检测（isRunning=false 或 mStopRequested=true）→ 安全退出
```

### 帧率限流设计（重要）

`new_sample` 回调在 GStreamer streaming 线程中执行。如果在此处睡眠等待，会耗尽 `mppvideodec` 的 buffer pool，导致硬件解码器死锁（表现为视频卡死）。

因此采用 **drop 模式**：无论是否推理，每帧都立即从 appsink 拉取并拿到数据，然后判断是否已到处理时间：
- **未到时间** → 直接 `gst_sample_unref()` 丢弃，立即返回
- **到时间** → 送入 analyzer 处理

这样 GStreamer 管道始终畅通，不会积压。

---

## 重连机制

| 场景 | 行为 |
|------|------|
| RTSP / USB 掉线（ERROR / 静默断流） | 阶梯退避无限重试（1→5→15→30 秒），恢复后首帧归零 |
| 文件播放完毕（EOS），`loop=true` | 重新创建管道从头播放，最多重试 3 次 |
| 文件播放完毕（EOS），`loop=false` | 退出 Bus 线程，不重连 |
| 程序退出（isRunning=false） | 立即中断所有等待，安全退出 |
| **流地址热切换**（config 变更） | config_monitor 停旧采集器 → 新地址重建，不走重连路径 |

重连时复用当前 Bus 线程（`start_thread=false`），避免重复创建监听线程造成竞态。
`stop()` 中断重连等待采用 `mStopRequested` 标志，可在 100ms 内打断最长 30 秒的退避睡眠。

---

## API 参考

### 构造与初始化

```cpp
// 输入源配置
struct SrcCfg_t {
    string srcType;       // "rtsp" / "file" / "usb"
    string location;      // RTSP URL / 文件路径 / 设备节点（如 /dev/video0）
    string videoEncType;  // "h264" 或 "h265"（仅 RTSP 有效）
    bool   loop = false;  // 本地文件是否循环播放
};

// 创建并初始化通道
DecChannel ch(chnId, srcCfg);
int ret = ch.init();      // 返回 0=成功，-1=失败
```

### 停止

```cpp
// 安全停止：设停止标志 → 等待 Bus 线程退出（最多 3 秒）→ 清理管道
ch.stop();
// 析构函数自动调用 stop()，一般不需要手动调用
```

### 状态查询

```cpp
ch.IsInited();            // 是否已初始化
ch.channelId();           // 通道 ID
ch.isLoop();              // 是否循环播放（动态读取配置，支持热重载）
ch.isStopRequested();     // 是否已请求停止
```

---

## 二次开发指南

### 新增输入源类型

1. 在 `decChannel.h` 中声明新成员变量（如 `mIsNewSrc`）
2. 在 `decChannel.cpp` 中仿照 `createUsbDecChannel` 实现新的 `createXxxDecChannel`
3. 在 `init()` 中根据 `srcType` 路由到新的创建函数
4. 在 `config.cpp` 的 `is_supported_src_type()` 中注册新类型名称（注意：`src_type` 必填、不再自动推断）
5. 在前端视频流节点「输入类型」下拉（`web_console/.../streamSource.ts` 的 `SRC_TYPES`）及配置文档中补充该类型

### 调整帧率限流

帧率由通道配置的 `playback_fps` 字段控制（`-1` 表示不限制）：

```json
{
  "channels": [
    {
      "stream": { "url": "rtsp://..." },
      "playback_fps": 15
    }
  ]
}
```

本地文件未配置 `playback_fps` 时，使用全局 `local_default_fps`（默认 25fps）。

### 修改重连等待时间

实时流(RTSP/USB)重连用**阶梯退避**：按已重连次数 `mReconnectCount` 取 `delay_sec` = 1 → 5 → 15 → 30 秒（见 `decChannel.cpp` 的 `reconnect()`；拉到首帧后 `resetReconnectCount()` 自动归零）。改这几档即可调等待：

```cpp
// decChannel.cpp reconnect()
if      (mReconnectCount == 0) delay_sec = 1;
else if (mReconnectCount == 1) delay_sec = 5;
else if (mReconnectCount == 2) delay_sec = 15;
else                           delay_sec = 30;
```

静默断流超时阈值在 `busListen` 函数中：

```cpp
uint64_t stall_threshold_us = pThis->mGstChn.is_file ? 3000000ULL : 15000000ULL;
// 文件流：3 秒无帧触发重连；实时流：15 秒无帧触发重连
```

### 配合显示与推理

采集模块调用 `videoOutHandle()` 向 analyzer 送帧，这是两个模块的唯一耦合点。采集模块本身不持有任何显示或推理资源。
