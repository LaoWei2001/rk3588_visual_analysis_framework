# 告警上传模块 (uploader/)

> 异步告警推送层：将业务逻辑产生的告警帧编码后写入 Redis 队列，由独立的 Python 微服务异步消费上报。

---

## 文件清单

| 文件 | 职责 |
|------|------|
| `alarm_uploader.h` | 初始化/反初始化接口、`alarm_uploader_enqueue`、`dify_uploader_enqueue` 声明 |
| `alarm_uploader.cpp` | Redis 连接管理、异步上传线程、JPEG 编码、队列写入实现 |

---

## 架构设计

```
业务逻辑（channel_logic / global_logic）
    │
    ├─ alarm_uploader_enqueue(img_draw, img_raw, camera_id, alarm_type)
    │       └─ 非阻塞入队（轻量投递，不等编码和网络）
    │
    └─ dify_uploader_enqueue(img, prompt, event_id)
            └─ 非阻塞入队

上传线程（内部异步线程，C++ 侧）
    ├─ 从内部队列取任务
    ├─ JPEG 编码（+ Base64）
    └─ Redis LPUSH → "server_queue" / "dify_queue"

Python 微服务（dist/services/upload/main.py）
    ├─ 线程 1：Redis BLPOP "server_queue" → HTTP POST 业务服务器
    └─ 线程 2：Redis BLPOP "dify_queue"  → 上传图片 + 触发 Dify 工作流
```

**关键设计**：C++ 侧只负责编码和写 Redis，完全不做网络 I/O，`enqueue` 调用的延迟在微秒级，不会阻塞推理线程。

---

## API 参考

### 生命周期

```cpp
// 初始化 Redis 连接和异步上传线程（main.cpp 中 app_ctrl_init 之后调用）
// 返回 int（0=成功）；无默认参数，host/port 由调用方显式传入
int alarm_uploader_init(const char *redis_host, int redis_port);

// 停止上传线程，释放 Redis 连接（main.cpp 退出流程中调用）
void alarm_uploader_deinit(void);
```

### 入队接口

```cpp
// 推送告警帧到服务器队列（server_queue）
// img_draw : 带标注框的帧（用于展示）
// img_raw  : 原始帧（用于存档/分析，可与 img_draw 相同）
// camera_id: 摄像头通道号
// alarm_type: 告警类型字符串（如 "intrusion"、"hook_tilt"）
// server_url: 该通道 HTTP 上报地址(方案2, 随消息进 Redis); nullptr/空=用上报服务默认值
int alarm_uploader_enqueue(const cv::Mat &img_draw,
                           const cv::Mat &img_raw,
                           int camera_id,
                           const char *alarm_type,
                           const char *server_url = nullptr);

// 推送到 Dify AI 分析队列（dify_queue）
// img      : 要分析的帧
// prompt   : 发送给 Dify 工作流的提示词
// event_id : 事件唯一标识（如 "ch0_f1234_t567890"）
// dify_api_url / dify_api_key: 该通道 Dify 地址与密钥(方案2, 随消息进 Redis); nullptr/空=用默认值
int dify_uploader_enqueue(const cv::Mat &img,
                          const char *prompt,
                          const char *event_id,
                          const char *dify_api_url = nullptr,
                          const char *dify_api_key = nullptr);
```

两个接口均为**非阻塞**，调用方无需等待网络完成。若内部队列已满（`ALARM_QUEUE_SIZE=64`），新任务会被丢弃并打印警告。

---

## Redis 数据格式

### server_queue 消息格式

```json
{
  "camera_id":    0,
  "alarm_type":   "intrusion",
  "snapTime":     "2026-05-08 12:00:00",
  "endTime":      "2026-05-08 12:00:00",
  "base64Data":   "<JPEG Base64，带标注框>",
  "base64DataRaw":"<JPEG Base64，原始帧>",
  "server_url":   "<该通道上报地址(方案2)，空=用上报服务默认值>"
}
```

### dify_queue 消息格式

```json
{
  "event_id":     "ch0_f1234_t567890",
  "base64Data":   "<JPEG Base64>",
  "prompt":       "分析画面中的违规情况",
  "dify_api_url": "<该通道 Dify 地址(方案2)，空=用默认>",
  "dify_api_key": "<该通道 Dify 密钥(方案2)，空=用默认>"
}
```

Dify 队列传递 Base64 图像，Python 微服务负责解码并调用 Dify 文件上传接口。
**方案2**：上报地址随消息走——通道在配置里填了 `server_url`/`dify_api_url`/`dify_api_key` 就用通道的，留空则回落到 Python 上报服务 `config.yaml` 里的默认值。

---

## 二次开发指南

### 在业务逻辑中触发上报

```cpp
#include "../uploader/alarm_uploader.h"

static void logic_my(ChannelContext *ctx) {
    if (!ctx || !ctx->results || !ctx->frame) return;
    for (auto &r : *ctx->results) {            // ctx->results 是指针，先解引用
        if (r.label != "person") continue;

        // *ctx->frame 是与 results 严格同源的帧，alarm_uploader 内部会 clone
        alarm_uploader_enqueue(*ctx->frame, *ctx->frame,
                               ctx->chnId, "person_detected");
        break; // 每帧最多报一次
    }
}
```

`alarm_uploader_enqueue` 内部会对图像做深拷贝，调用方无需自行 clone。

### 修改 Redis 连接参数

在 `main.cpp` 中修改 `alarm_uploader_init` 的参数：

```cpp
// main.cpp
alarm_uploader_init("192.168.1.100", 6379);  // 指定远端 Redis
```

或修改 Python 微服务的 `config.yaml`（C++ 侧与 Python 侧需保持一致）。

### 修改队列容量

在 `constants.h` 中修改：

```cpp
namespace constants {
    constexpr int ALARM_QUEUE_SIZE = 64;  // 改为更大的值
}
```

### 修改 JPEG 编码质量

在 `alarm_uploader.cpp` 中找到编码参数并修改：

```cpp
vector<int> encode_params = {cv::IMWRITE_JPEG_QUALITY, 85};  // 改为 60~95
```

### 关闭上传（调试时）

可将 `alarm_uploader_init` 的调用注释掉，所有 `enqueue` 调用在 Redis 未连接时会静默失败，不影响推理和显示。
