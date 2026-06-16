# 告警上传模块 (uploader/)

> 异步告警推送层：业务逻辑产生告警后**非阻塞投递**，C++ 内部上传线程异步处理。**server 告警落地到本地「发件箱」目录**（断网攒着、网络恢复边传边删），**Dify 分析走 Redis 队列**；真正的网络上报由独立 Python 微服务完成。

---

## 文件清单

| 文件 | 职责 |
|------|------|
| `alarm_uploader.h` | 初始化/反初始化接口、`alarm_uploader_enqueue`、`dify_uploader_enqueue` 声明 |
| `alarm_uploader.cpp` | 内部队列 + 异步上传线程；server 告警**落盘到发件箱**、Dify 走 **Redis**、JPEG 编码、Redis 连接管理 |

---

## 架构设计

```
业务逻辑（channel_logic / global_logic）
    ├─ alarm_uploader_enqueue(img_draw, img_raw, camera_id, alarm_type, server_url)
    │       └─ 非阻塞入队 g_alarm_queue（上限 32，满则丢最老）
    └─ dify_uploader_enqueue(img, prompt, event_id, dify_api_url, dify_api_key)
            └─ 非阻塞入队 g_dify_queue（上限 16，满则丢最老）

上传线程（C++ 内部单线程，从两个内存队列取任务）
    ├─ server 告警 → record_alarm_local()：把 带框图.jpg + 原图_raw.jpg + .json 元数据
    │       写进本地发件箱目录 alarm_store/（先写 .json.tmp 再 rename 原子提交；
    │       目录占用超 100MB 时从最老记录开始删）——**不走 Redis、不做网络 I/O**
    └─ Dify 分析  → redis_rpush("dify_queue")：JPEG→Base64 内嵌进消息推入 Redis

Python 微服务（service/upload/main.py，部署后在 <App>/services/upload/）
    ├─ OutboxForwarder 线程：扫 alarm_store/ → 读 .jpg 转 Base64 → HTTP POST 业务服务器
    │       传成功即删本地记录；服务器拒绝/断网则保留，下轮重试（store-and-forward）
    └─ DifyUploader 线程：Redis BLPOP "dify_queue" → 上传图片 + 触发 Dify 工作流
```

**关键设计**：
- `enqueue` 只做图像深拷贝 + 入内存队列，延迟微秒级，**绝不阻塞推理/逻辑线程**。
- **server 告警先无条件落盘**：即便业务服务器宕机或断网，告警也不丢——攒在本地发件箱，Python 侧网络恢复后逐条补传、**传一条删一条**。
- **Dify 仍走 Redis**：分析类请求无需断网留存，直接 base64 进队列实时消费。

---

## API 参考

### 生命周期

```cpp
// 初始化 Redis 连接（供 Dify 路径用）和异步上传线程（main.cpp 中 app_ctrl_init 之后调用）
// 返回 int（0=成功）；无默认参数，host/port 由调用方显式传入
int alarm_uploader_init(const char *redis_host, int redis_port);

// 停止上传线程，释放 Redis 连接（main.cpp 退出流程中调用）
void alarm_uploader_deinit(void);
```

### 入队接口

```cpp
// 推送 server 告警 → 落地到本地发件箱（非 Redis；Python OutboxForwarder 负责补传）
// img_draw : 带标注框的帧（落盘为 <base>.jpg）
// img_raw  : 原始帧（落盘为 <base>_raw.jpg，可与 img_draw 相同；空则不写原图）
// camera_id: 摄像头通道号
// alarm_type: 告警类型字符串（如 "intrusion"、"hook_tilt"）
// server_url: 该通道 HTTP 上报地址(方案2, 随记录进 .json); nullptr/空=用上报服务默认值
int alarm_uploader_enqueue(const cv::Mat &img_draw,
                           const cv::Mat &img_raw,
                           int camera_id,
                           const char *alarm_type,
                           const char *server_url = nullptr);

// 推送到 Dify AI 分析队列（Redis dify_queue）
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

两个接口均为**非阻塞**，调用方无需等待落盘/网络。内部队列满（server `g_alarm_queue` 上限 **32**、dify `g_dify_queue` 上限 **16**，均为 enqueue 函数内的 `static constexpr` 局部常量）时，丢弃**最老**任务并打印警告。

---

## 数据格式

### server 告警 — 本地发件箱（`alarm_store/`，每条 3 个文件）

每条告警落盘三个文件（`<base>` 形如 `ch0_<时间戳>`）：

| 文件 | 内容 |
|------|------|
| `<base>.jpg` | 带标注框的帧（JPEG 质量 90） |
| `<base>_raw.jpg` | 原始帧（`img_raw` 为空时不写） |
| `<base>.json` | 元数据（先写 `.json.tmp` 再 `rename` 原子提交：`.json` 一出现即代表整条记录就绪） |

`.json` 字段：
```json
{
  "camera_id":  0,
  "alarm_type": "intrusion",
  "snapTime":   "2026-05-08 12:00:00",
  "endTime":    "2026-05-08 12:00:00",
  "server_url": "<该通道上报地址(方案2)，空=用上报服务默认值>",
  "img":        "<带框图文件名，如 ch0_xxx.jpg>",
  "img_raw":    "<原图文件名，可空>",
  "ts":         1715155200
}
```
> 注意：图片是**独立的 .jpg 文件**，`.json` 里只存**文件名引用**（`img` / `img_raw`）；Base64 由 Python 在转发时现读现编码，**不是 C++ 内嵌**。发件箱目录由环境变量 `ALARM_STORE_DIR` 指定（默认 `./alarm_store`），占用上限 `ALARM_STORE_CAP_BYTES`（100 MB，超出从最老记录开始删）。

### dify_queue — Redis 消息（Base64 内嵌）

```json
{
  "event_id":     "ch0_f1234_t567890",
  "base64Data":   "<JPEG Base64>",
  "prompt":       "分析画面中的违规情况",
  "dify_api_url": "<该通道 Dify 地址(方案2)，空=用默认>",
  "dify_api_key": "<该通道 Dify 密钥(方案2)，空=用默认>"
}
```

**方案2**：上报地址随记录/消息走——通道在配置里填了 `server_url`/`dify_api_url`/`dify_api_key` 就用通道的，留空则回落到 Python 上报服务 `config.yaml` 里的默认值。

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

`alarm_uploader_enqueue` 内部会对图像做深拷贝，调用方无需自行 clone。**务必限频**（上报很贵）：用 `ctx->timestamp_ms` 卡间隔或用闩锁，范例见 channel-logic skill 的 `examples/logic_server.md` / `examples/logic_hook.md`。

### 修改队列容量

容量是 enqueue 函数内的局部 `static constexpr`，直接改对应常量：

```cpp
// alarm_uploader.cpp
int alarm_uploader_enqueue(...) {
    static constexpr size_t MAX_QUEUE_SIZE = 32;   // server 内存队列上限
    ...
}
int dify_uploader_enqueue(...) {
    static constexpr size_t MAX_DIFY_QUEUE = 16;   // dify 内存队列上限
    ...
}
```

### 修改 JPEG 编码质量

落盘图质量在 `record_alarm_local`：

```cpp
std::vector<int> jpg_params = {cv::IMWRITE_JPEG_QUALITY, 90};  // 改为 60~95
```

### 修改发件箱目录 / 容量上限

- 目录：环境变量 `ALARM_STORE_DIR`（systemd 单元里设；默认 `./alarm_store`）。Python 上报服务要读同一目录（其 `ALARM_STORE_DIR` 或默认 `<app>/alarm_store`），**两侧必须一致**。
- 上限：`alarm_uploader.cpp` 的 `ALARM_STORE_CAP_BYTES`（默认 100 MB）。

### 修改 Redis 连接参数（影响 Dify 路径）

在 `main.cpp` 中修改 `alarm_uploader_init` 的参数：

```cpp
alarm_uploader_init("192.168.1.100", 6379);  // 指定远端 Redis（Dify 用）
```

或修改 Python 微服务的 `config.yaml`（C++ 侧与 Python 侧 Redis 配置需一致）。

### 关闭上传（调试时）

可将 `alarm_uploader_init` 的调用注释掉：Dify 路径在 Redis 未连接时静默失败；**server 告警仍会落盘到发件箱**（不依赖 Redis），只是没有 Python 服务时不会被补传出去。两者都不影响推理和显示。
