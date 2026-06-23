# 业务逻辑二次开发说明书

> 适用版本：rk3588_yolo (schema_version = 2)
>
> 本文档覆盖两种扩展场景：
> - **通道逻辑**（单路，每帧推理后同步调用）
> - **全局逻辑**（跨路，独立线程轮询）
>
> 以及**添加可热重载配置参数**的完整流程。

---

## 目录

1. [通道逻辑 — 单路每帧](#1-通道逻辑--单路每帧)
   - [1.1 接入三步](#11-接入三步)
   - [1.2 ChannelContext 字段速查](#12-channelcontext-字段速查)
   - [1.3 AlgoResult 字段速查](#13-algoresult-字段速查)
   - [1.4 跨帧持久状态](#14-跨帧持久状态)
   - [1.5 绘制](#15-绘制)
   - [1.6 告警上报](#16-告警上报)
   - [1.7 读取配置参数](#17-读取配置参数)
   - [1.8 ROI 判断](#18-roi-判断)
2. [全局逻辑 — 跨路轮询](#2-全局逻辑--跨路轮询)
   - [2.1 接入三步](#21-接入三步)
   - [2.2 GlobalContext 字段速查](#22-globalcontext-字段速查)
   - [2.3 跨通道取数](#23-跨通道取数)
   - [2.4 读取通道专有状态](#24-读取通道专有状态)
   - [2.5 其他便捷接口](#25-其他便捷接口)
3. [添加可热重载配置参数](#3-添加可热重载配置参数)
   - [3.1 通道参数](#31-通道参数)
   - [3.2 全局参数](#32-全局参数)
   - [3.3 支持的参数类型](#33-支持的参数类型)
   - [3.4 热重载说明](#34-热重载说明)
4. [完整示例：烟雾检测告警](#4-完整示例烟雾检测告警)
5. [附录：常见问题](#5-附录常见问题)

---

## 1. 通道逻辑 — 单路每帧

### 1.1 接入三步

通道逻辑是**每路视频、每帧推理完成后同步调用**的回调函数。框架负责推理、跟踪、图像缩放，你只管业务判断。

> **架构**：每个通道逻辑放在**独立的 `src/logic/logic_xxx.cpp` 文件**里，文件末尾用
> `REGISTER_LOGIC` 宏**自注册**（在 `main()` 之前自动登记到分发表）。
> `src/logic` 下的 `.cpp` 由 CMake (`aux_source_directory`) 自动收集编译，
> 因此**新增一个逻辑只需新增一个文件、删除一个逻辑只需删掉对应文件**，不牵连任何其它文件。
> `channel_logic.cpp` 只保留框架核心（`ChannelContext` 方法 / `draw_*` / 分发表），不再放具体逻辑。
>
> **逻辑的唯一"身份"是 `REGISTER_LOGIC` 注册的那个字符串**（文件名 / 函数名只是约定，对外不可见）：它必须 == `config.json` 的 `logic`（运行时据此查分发表跑哪个函数）== `logics.json` 的 `name`（网页据 `logics.json` 列出可选逻辑、并据此渲染参数；网页「逻辑名称」只能下拉选、不能手填）。本节只讲"怎么用"，完整的**四名关系 / 网页如何识别 / 名字失配会怎样**见 `rk3588-channel-logic` skill 的 `references/logic-naming-and-registration.md`。

#### 第一步：新建 `src/logic/logic_my_detect.cpp`

```cpp
#include "logic_common.h"   // 一行包含所有 logic 常用头（ChannelContext / draw_* / 上报 / 状态结构…）

static void logic_my_detect(ChannelContext *ctx)
{
    if (!ctx) return;
    // 在此实现业务逻辑
}

REGISTER_LOGIC("logic_my_detect", logic_my_detect);  // ← 自注册，无需改动任何其它文件
```

#### 第二步：重新生成构建（让 CMake 收录新文件）

`aux_source_directory` 在 **configure 阶段**收集文件，新增/删除 `.cpp` 后需让 CMake 重新 configure：

```bash
cmake -S . -B build      # 或 touch CMakeLists.txt 触发重配；干净起见也可删 build 重建
cmake --build build
```

> 删除一个逻辑：直接 `rm src/logic/logic_xxx.cpp`，同样重新 configure 即可，自注册随文件一起消失。

#### 第三步：在 `config.json` 指定通道使用该逻辑

```json
{
  "channels": [
    {
      "id": 0,
      "stream": { "url": "rtsp://192.168.1.100:554/stream" },
      "logic": "logic_my_detect"
    }
  ]
}
```

修改 `logic` 字段后**无需重启**，约 4 秒内热重载生效。

---

### 1.2 ChannelContext 字段速查

| 字段 | 类型 | 说明 |
|---|---|---|
| `ctx->chnId` | `int` | 当前通道号 |
| `ctx->frame` | `const cv::Mat *` | 当帧 BGR 图，640×640（模型输入尺寸），上报用这张 |
| `ctx->frame_id` | `int64_t` | 当帧序号，单调递增 |
| `ctx->timestamp_ms` | `uint64_t` | 单调时钟(steady_clock，毫秒)，只用于算间隔，**不是日历时间** |
| `ctx->unix_ms` | `uint64_t` | 墙上时钟 Unix epoch 毫秒（真实时间，RTSP/USB/文件 三源统一）；配 `time_hms()`/`time_str()`/`datetime()` |
| `ctx->dt_ms` | `float` | 距上一帧的时间间隔（毫秒），用于积分/计时 |
| `ctx->results` | `std::vector<AlgoResult> *` | 当帧所有检测框，可修改（如改 `box_color`） |
| `ctx->roi` | `const std::vector<cv::Point> *` | 兼容字段：第 0 个 ROI 多边形（640 坐标系），无 ROI 时为空 |
| `ctx->rois` | `const std::vector<RoiZone> *` | 本通道**全部** ROI 区域（多区域，各含名字+多边形）；配 `roi_count`/`roi_by_name`/`roi_polygon_at`/`roi_index_of` 等 |
| `ctx->config` | `const ChannelConfig *` | 本通道配置（只读），可读自定义参数 |
| `ctx->draw_cmds` | `std::vector<DrawCommand> *` | 向此 push 绘制指令，框架自动渲染到屏幕和/或上报图 |
| `ctx->state` | `std::shared_ptr<void> *` | 跨帧持久状态，详见 [1.4 节](#14-跨帧持久状态) |
| `ctx->infer_enabled` | `int` | 本通道是否有 NPU 推理（0 = 纯显示通道） |
| `ctx->infer_fps` | `float` | 当前推理帧率 |
| `ctx->disp_fps` | `float` | 当前显示帧率 |

**内置便捷方法**（免去手动遍历 results）：

```cpp
ctx->has_target("person")                           // 整帧是否存在指定类别
ctx->target_count("car")                            // 整帧指定类别数量
roi_contains(ctx, some_box, ROI_ALL)                // 框是否在任一 ROI 内（没画区域=整帧）
roi_has_target(ctx, "person", ROI_ALL)              // 任一 ROI 内是否有该类别
roi_count_target(ctx, "car", ROI_ALL)               // 所有 ROI 内该类别数量（并集）
roi_count_target(ctx, "car", 1)                     // 只看第 1 个区域（传序号）
roi_count_target(ctx, "car", roi_find(ctx, "门口")) // 只看名为"门口"的区域（先 roi_find 取序号）
ctx->time_hms()                                     // 本帧真实时间 "HH:MM:SS"(三源统一); ctx->unix_ms=epoch 毫秒, ctx->time_str()="YYYY-MM-DD HH:MM:SS"
ctx->datetime()                                     // 拆成年月日时分秒: FrameTime{year,month,day,hour,minute,second,millis} 各是 int
ctx->snapshot()                                     // 深拷贝当帧（返回 cv::Mat）
```

**跨通道查询**（在通道逻辑内安全取另一路数据）：

```cpp
ChannelSnapshot snap = ctx->get_channel_snapshot(2);  // 取通道 2 的快照
```

详见 [2.3 节](#23-跨通道取数)，接口与全局逻辑一致。

---

### 1.3 AlgoResult 字段速查

`ctx->results` 是 `std::vector<AlgoResult>`，每个元素代表一个检测目标：

```cpp
for (auto &r : *ctx->results)
{
    r.label          // std::string：类别名，如 "person"、"car"
    r.class_id       // int：类别 ID（对应 labels.txt 行号）
    r.score          // float：置信度 0.0~1.0
    r.box            // cv::Rect：检测框 (x, y, width, height)，640 坐标系
    r.track_id       // int：跟踪 ID；-1 = 未稳定跟踪，>= 0 = 跨帧稳定 ID
    r.frame_id       // int64_t：产生此结果的帧序号
    r.box_color      // cv::Scalar：可修改，(-1,-1,-1) 时框架用默认颜色

    r.box_center()               // cv::Point：返回框中心点
    r.box_contains(cv::Point)    // bool：点是否在框内
    r.dist_sq_to(cv::Point(x,y)) // int：框中心到指定点的距离平方（免 sqrt）

    // 模型特有字段（按实际模型类型有值）：
    r.keypoints      // std::vector<cv::Point2f>：pose 模型关键点
    r.boxMask        // cv::Mat：seg 模型掩码
    r.vx, r.vy       // float：卡尔曼速度估计（像素/帧），track_id >= 0 时有效
}
```

---

### 1.4 跨帧持久状态

每次函数调用之间需要保存状态（计时器、报警锁存、历史 ID 集合等）时使用 `ctx->state`：

```cpp
// 第一步：定义状态结构体（推荐放在函数前面）
struct MyState
{
    uint64_t last_alarm_ms = 0;
    bool     alarm_latched = false;
    int      event_count   = 0;
};

static void logic_my(ChannelContext *ctx)
{
    // 第二步：首次进入时分配，之后每帧拿到同一个实例
    if (!*ctx->state)
        *ctx->state = std::make_shared<MyState>();
    auto &s = *std::static_pointer_cast<MyState>(*ctx->state);

    // 第三步：正常读写
    s.event_count++;
    if (ctx->timestamp_ms - s.last_alarm_ms > 30000)
    {
        s.alarm_latched = false;
    }
}
```

> **注意**：切换通道的 `logic` 名称（热重载）会销毁旧状态对象、创建新状态对象，计时器等会归零。

> **千万别用 `static` / 全局变量存跨帧状态**：`logic_my` 这个函数被所有通道复用，`static` 变量整个进程只有一份，会被所有通道共享、互相**串台**。每通道独立的跨帧状态**只能**放 `ctx->state`——框架按通道号 `chnId` 给每个通道分了独立一格（`channels_state[chnId].logic_state`），`ctx` 每帧栈上现搭、只指向本通道那一格，所以"同名不同值、互不干扰"天然成立。完整隔离机制见 `rk3588-channel-logic` skill 的 `channelcontext-api.md`「变量隔离」节。

---

### 1.5 绘制

四个辅助函数向 `draw_cmds` 推入绘制指令，框架自动渲染。`target` 参数控制目的地：

| `target` 值 | 含义 |
|---|---|
| `DrawCommand::DISPLAY` | 只画到屏幕 |
| `DrawCommand::UPLOAD` | 只画到上报图 |
| `DrawCommand::ALL`（默认）| 屏幕和上报图都画 |

```cpp
draw_rect(ctx,
    cv::Rect(x, y, w, h),
    cv::Scalar(0, 255, 0),   // BGR 颜色
    2,                        // 线宽
    DrawCommand::ALL);

draw_circle(ctx, cv::Point(cx, cy), radius,
    cv::Scalar(0, 0, 255), 2);

draw_line(ctx, cv::Point(x1, y1), cv::Point(x2, y2),
    cv::Scalar(255, 255, 0), 2);

draw_text(ctx, "ALARM!",
    cv::Point(20, 40),
    cv::Scalar(0, 0, 255),   // BGR 颜色
    0.7,                      // font_scale
    2,                        // thickness
    DrawCommand::DISPLAY);    // 只显示，不叠到上报图
```

也可直接修改检测框颜色，框架在渲染时使用：

```cpp
for (auto &r : *ctx->results)
    if (r.label == "fire")
        r.box_color = cv::Scalar(0, 0, 255);  // 红色
```

---

### 1.6 告警上报

#### 上报到业务服务器（Redis → Python 微服务 → HTTP POST）

```cpp
// img_draw：叠加了文字/框的展示图
// img_raw ：原始帧（不含叠加）
alarm_uploader_enqueue(img_draw, *ctx->frame, ctx->chnId, "fire_detected");
```

#### 上报到 Dify（AI 二次分析）

```cpp
dify_uploader_enqueue(img_draw, "请判断画面是否有火情", "event_id_xxx");
```

#### 完整的"克隆 → 叠加 → 上报"写法

```cpp
if (ctx->frame && !ctx->frame->empty())
{
    cv::Mat upload_img = ctx->frame->clone();

    // 把 draw_cmds 里 target=UPLOAD 的指令渲染到 upload_img
    RenderParams rp    = ctx->render_params();
    rp.show_fps        = 0;                      // 上报图不带 FPS 文字
    rp.target_mask     = DrawCommand::UPLOAD;    // 只渲染 UPLOAD 指令
    render_overlays(upload_img, rp);

    alarm_uploader_enqueue(upload_img, *ctx->frame, ctx->chnId, "my_alarm");
}
```

两个上报接口均为**非阻塞**，推入队列后立即返回，由 `upload_worker` 线程异步处理。

---

### 1.7 读取配置参数

```cpp
// 读本通道配置（ctx->config 是当前热重载后的最新值）
float thresh   = ctx->config->obj_thresh;         // <0 时应改用全局值
int   radius   = ctx->config->radius;             // 内置示例自定义字段
float duration = ctx->config->alarm_duration_sec; // 你新增的字段（见第 3 节）

// 读全局配置（需要加读锁）
pthread_rwlock_rdlock(&g_pCtrl->mtx);
int max_fps = g_pCtrl->config.max_fps;
pthread_rwlock_unlock(&g_pCtrl->mtx);
```

---

### 1.8 ROI 判断

ROI 多边形顶点已由框架自动缩放到 640 坐标系，直接使用即可：

```cpp
int has_roi = (ctx->roi && ctx->roi->size() >= 3);

// 遍历结果，跳过 ROI 外的目标
if (ctx->results)
{
    for (auto &r : *ctx->results)
    {
        if (has_roi &&
            cv::pointPolygonTest(*ctx->roi, r.box_center(), false) < 0)
            continue;  // 目标在 ROI 外

        // 处理 ROI 内的目标 ...
    }
}

// 画 ROI 边框
if (has_roi)
{
    for (size_t i = 0; i < ctx->roi->size(); ++i)
    {
        const cv::Point &p1 = (*ctx->roi)[i];
        const cv::Point &p2 = (*ctx->roi)[(i + 1) % ctx->roi->size()];
        draw_line(ctx, p1, p2, cv::Scalar(0, 255, 255), 2);
    }
}
```

---

## 2. 全局逻辑 — 跨路轮询

### 2.1 接入三步

全局逻辑在**独立的 pthread 线程**中按固定间隔（默认 100ms）循环调用，专门用于跨通道聚合多路数据，适合"≥2 路同时出现人员则联动告警"等场景。

#### 第一步：在 `src/logic/global_logic.cpp` 实现函数

在文件末尾、`global_logic_register()` 前面添加：

```cpp
static void global_my_multizone(GlobalContext *gctx)
{
    if (!gctx) return;
    // 在此实现跨通道逻辑
}
```

#### 第二步：在同文件的 `global_logic_register()` 中注册

```cpp
static void global_logic_register(void)
{
    register_global_logic("global_example",      global_example);
    register_global_logic("global_default",       global_default);
    register_global_logic("global_my_multizone", global_my_multizone);  // ← 新增
}
```

#### 第三步：在 `config.json` 的 `global_logics` 数组中配置

```json
{
  "global_logics": [
    {
      "enable": true,
      "logic": "global_my_multizone",
      "channels": [0, 1, 2],
      "poll_interval_ms": 100
    }
  ]
}
```

`channels` 为空数组时监控所有通道。支持配置多个并行实例（数组添加多项即可）。

---

### 2.2 GlobalContext 字段速查

| 字段 | 类型 | 说明 |
|---|---|---|
| `gctx->config` | `const GlobalLogicConfig *` | 本实例配置（channels、poll_interval_ms 等） |
| `gctx->timestamp_ms` | `uint64_t` | 当次 tick 的时间戳（毫秒） |
| `gctx->tick_id` | `int64_t` | 单调递增 tick 计数，可用于节流（`tick_id % 50 == 0`） |
| `gctx->has_new_infer` | `int` | 本次 tick 期间是否有通道完成了新推理（1 = 有）|
| `gctx->latest_infer_channel` | `int` | 最新完成推理的通道号（-1 = 无）|
| `gctx->latest_infer_ts_ms` | `uint64_t` | 最新推理完成的时间戳 |
| `gctx->state` | `std::shared_ptr<void> *` | 跨 tick 持久状态，用法与通道逻辑的 state 完全相同 |

---

### 2.3 跨通道取数

**唯一正确的取数方式**是 `get_channel_snapshot()`——一次持锁原子读出，`frame` 与 `results` 保证来自**同一帧**：

```cpp
gctx->for_each_channel([&](int chnId, int /*idx*/)
{
    ChannelSnapshot snap = gctx->get_channel_snapshot(chnId);

    if (snap.frame.empty())         return;  // 通道尚未出帧
    if (snap.result_age_ms > 500)   return;  // 数据超过 500ms，太旧跳过

    // snap.frame      — BGR, 640×640，与 results 严格同帧
    // snap.results    — 当帧所有检测框
    // snap.frame_seq  — 帧序号，可用于调试确认同帧
    // snap.infer_fps  — 该通道推理帧率
    // snap.disp_fps   — 该通道显示帧率
    // snap.logic_state — 该通道 channel_logic 的 state（需 static_pointer_cast）
    // snap.has_results — 是否有检测结果

    for (const auto &r : snap.results)
    {
        if (r.label == "person")
            printf("CH%d: person, score=%.2f, box=(%d,%d)\n",
                   chnId, r.score, r.box.x, r.box.y);
    }
});
```

> **不要**分两次调用分别取 `frame` 和 `results`——两次调用之间数据可能已更新，会拿到不同时刻的内容。一次 `snapshot` 全部取出。

---

### 2.4 读取通道专有状态

全局逻辑可以读取某通道 channel_logic 的跨帧状态（`PersonAlarmState`、`HookState` 等），前提是状态结构体定义在 `logic_tools.h` 中（或你自己的共享头文件里）：

```cpp
// 示例：读取 logic_person_alarm 的状态
gctx->for_each_channel([&](int chnId, int)
{
    if (!gctx->channel_has_logic(chnId, "logic_person_alarm"))
        return;

    ChannelSnapshot snap = gctx->get_channel_snapshot(chnId);
    if (snap.frame.empty() || snap.result_age_ms > 500)
        return;

    // 把通用 shared_ptr<void> 转换成具体类型
    auto st = std::static_pointer_cast<PersonAlarmState>(snap.logic_state);
    if (st && st->person_detected)
    {
        printf("CH%d: %d person(s) detected\n", chnId, st->person_count);
        // 可以用 snap.frame + snap.results 上报
    }
});
```

如果你的通道逻辑有自定义状态结构体，把它的定义放到 `logic_tools.h`（或新建一个共享头文件），全局逻辑 `#include` 后即可访问。

---

### 2.5 其他便捷接口

不需要完整快照时，可用轻量查询接口：

```cpp
// 指定通道 200ms 内是否有指定类别目标
gctx->channel_has_target(chnId, "person", 200)

// 指定通道 200ms 内指定类别目标数量
gctx->get_channel_target_count(chnId, "car", 200)

// 各通道帧率
gctx->get_channel_infer_fps(chnId)
gctx->get_channel_disp_fps(chnId)

// 判断通道当前运行的 logic 名称
gctx->channel_has_logic(chnId, "logic_person_alarm")  // true/false
gctx->get_channel_logic_name(chnId)                   // 返回名称字符串
```

---

## 3. 添加可热重载配置参数

参数分两类：**通道参数**（每通道独立）和**全局参数**（所有通道共享）。两者都自动支持 JSON 解析和热重载，只需修改两个文件。

> **"通道参数每通道独立"的含义**：`ChannelConfig` 字段只定义**一次**，但每通道各有一个 `ChannelConfig` 实例，所以同名字段在各通道是**独立的值**（`channels[0].x` ≠ `channels[1].x`）。任何 logic 都能读 `ctx->config->x`，但读到的恒为**本通道**那一份（该通道 config.json 没配就是结构体默认值）；`ctx->config` 读不到别的通道的值。想跨通道共享同一个值，要么每通道都配，要么做成全局参数。
> 另外 `ctx->config` 是**只读**的：逻辑运行中要**修改**的状态（计数/计时/闩锁等）放 `ctx->state`，**不要**塞进 ChannelConfig——它是只读的，且热重载 `sync_fields` 会用 config.json 的值把你写的覆盖回去。

### 3.1 通道参数

**示例**：添加 `alarm_duration_sec`（报警确认时长，float，每通道独立）。

**① 在 `src/config/config.h` 的 `ChannelConfig` 结构体加字段：**

```cpp
struct ChannelConfig
{
    // ... 已有字段 ...

    /* 自定义逻辑中的变量 */
    int   radius = 1;                // 已有示例字段
    float alarm_duration_sec = 2.0f; // ← 新增，默认 2 秒
    int   alarm_cooldown_sec = 60;   // ← 新增，默认 60 秒
};
```

**② 在 `src/config/config_init.cpp` 的 `init_config_fields()` 注册：**

```cpp
// 自定义逻辑中的变量配置区域
REG_C("radius",             INT,   radius);
REG_C("dify_prompt",        STRING, dify_prompt);
REG_C("alarm_duration_sec", FLOAT,  alarm_duration_sec);  // ← 新增
REG_C("alarm_cooldown_sec", INT,    alarm_cooldown_sec);   // ← 新增
```

**③ 在逻辑函数中读取（热重载透明生效）：**

```cpp
float duration = ctx->config->alarm_duration_sec;
int   cooldown = ctx->config->alarm_cooldown_sec;
```

**④ 在 `config.json` 对应通道中添加（可选，不填则用默认值）：**

```json
{
  "id": 0,
  "logic": "logic_my",
  "alarm_duration_sec": 3.5,
  "alarm_cooldown_sec": 120
}
```

---

### 3.2 全局参数

**示例**：添加 `multi_zone_cooldown_ms`（整型，全局共享）。

**① 在 `src/config/config.h` 的 `AppConfig` 结构体加字段：**

```cpp
struct AppConfig
{
    // ... 已有字段 ...
    int multi_zone_cooldown_ms = 30000;  // ← 新增，默认 30 秒
};
```

**② 在 `src/config/config_init.cpp` 注册（用 `REG_G` 而不是 `REG_C`）：**

```cpp
REG_G("multi_zone_cooldown_ms", INT, multi_zone_cooldown_ms);
```

**③ 在逻辑函数中读取（需要加读锁）：**

```cpp
// 通道逻辑或全局逻辑中均可
pthread_rwlock_rdlock(&g_pCtrl->mtx);
int cooldown = g_pCtrl->config.multi_zone_cooldown_ms;
pthread_rwlock_unlock(&g_pCtrl->mtx);
```

**④ 在 `config.json` 的 `global` 段添加：**

```json
{
  "global": {
    "model_path": "assets/yolov8n.rknn",
    "multi_zone_cooldown_ms": 60000
  }
}
```

---

### 3.3 支持的参数类型

| 类型宏 | C++ 类型 | JSON 示例 |
|---|---|---|
| `INT` | `int` | `"max_fps": 15` |
| `FLOAT` | `float` | `"obj_thresh": 0.5` |
| `BOOL` | `bool` | `"enable_display": true` |
| `STRING` | `std::string` | `"logic": "logic_xxx"` |
| `STRING_ARRAY` | `std::vector<std::string>` | `"detect_classes": ["person", "car"]` |

---

### 3.4 热重载说明

- `config_monitor` 线程每 **2 秒**轮询配置文件变化，修改 `config.json` 后约 **2~4 秒**自动生效
- **支持热重载**：所有通过 `REG_G` / `REG_C` 注册的字段，包括你新增的自定义字段
- **不支持热重载**（需重启）：通道数量、RTSP/USB 地址、通道 `enable` 字段、`infer_enable`（推理开关，改后需停止再启动以重新分配推理/分发线程）
- **模型路径变化**：会触发模型热重载（下载新模型后自动切换），无需重启
- 开启 `"debug_display": true` 可在日志中看到热重载触发的详细信息

---

## 4. 完整示例：烟雾检测告警

**需求**：检测到 `smoke` 类别目标后，持续 N 秒方触发告警（避免误报），冷却 M 秒后才能再次触发。N 和 M 从配置文件读取，支持热调整。告警图叠加检测框后上报到业务服务器。

### 第一步：添加配置参数

**`src/config/config.h`** — `ChannelConfig` 加两个字段：

```cpp
float smoke_confirm_sec  = 3.0f;   // 烟雾确认时长（秒），默认 3 秒
int   smoke_cooldown_sec = 60;     // 报警冷却时长（秒），默认 60 秒
```

**`src/config/config_init.cpp`** — `init_config_fields()` 末尾注册：

```cpp
REG_C("smoke_confirm_sec",  FLOAT, smoke_confirm_sec);
REG_C("smoke_cooldown_sec", INT,   smoke_cooldown_sec);
```

### 第二步：实现通道逻辑

新建 **`src/logic/logic_smoke_detect.cpp`**（顶部 `#include "logic_common.h"`，末尾自注册）：

```cpp
#include "logic_common.h"

/*======================== logic_smoke_detect — 烟雾检测告警 ========================*/

struct SmokeState
{
    bool     smoke_active   = false;   // 当前帧是否持续检测到烟雾
    uint64_t smoke_start_ms = 0;       // 本次烟雾开始时刻
    bool     alarm_sent     = false;   // 报警锁存（冷却中不重复发）
    uint64_t last_alarm_ms  = 0;       // 最近一次报警时刻
};

static void logic_smoke_detect(ChannelContext *ctx)
{
    if (!ctx) return;

    /* ---- 初始化跨帧状态 ---- */
    if (!*ctx->state)
        *ctx->state = std::make_shared<SmokeState>();
    auto &s = *std::static_pointer_cast<SmokeState>(*ctx->state);

    /* ---- 从配置读参数（热重载透明生效）---- */
    float    confirm_sec  = ctx->config ? ctx->config->smoke_confirm_sec  : 3.0f;
    uint64_t cooldown_ms  = (uint64_t)(ctx->config ? ctx->config->smoke_cooldown_sec : 60) * 1000;

    /* ---- 检测当帧 ROI 内是否有烟雾 ---- */
    bool smoke_now = roi_has_target(ctx, "smoke", ROI_ALL);

    if (smoke_now)
    {
        if (!s.smoke_active)
        {
            /* 新一轮烟雾开始 */
            s.smoke_active   = true;
            s.smoke_start_ms = ctx->timestamp_ms;
        }
    }
    else
    {
        s.smoke_active = false;
        if (!s.alarm_sent)
            s.smoke_start_ms = 0;  /* 未触发报警则清零计时 */
    }

    /* ---- 冷却结束后解除锁存 ---- */
    if (s.alarm_sent &&
        ctx->timestamp_ms - s.last_alarm_ms >= cooldown_ms)
    {
        s.alarm_sent    = false;
        s.smoke_start_ms = 0;
    }

    /* ---- 确认时长到达 → 上报 ---- */
    if (s.smoke_active && !s.alarm_sent)
    {
        float elapsed_sec = (ctx->timestamp_ms - s.smoke_start_ms) / 1000.0f;
        if (elapsed_sec >= confirm_sec && ctx->frame && !ctx->frame->empty())
        {
            /* 克隆帧，把 UPLOAD 指令渲染上去后上报 */
            cv::Mat upload_img = ctx->frame->clone();
            RenderParams rp    = ctx->render_params();
            rp.show_fps        = 0;
            rp.target_mask     = DrawCommand::UPLOAD;
            render_overlays(upload_img, rp);

            alarm_uploader_enqueue(upload_img, *ctx->frame, ctx->chnId, "smoke");

            s.alarm_sent    = true;
            s.last_alarm_ms = ctx->timestamp_ms;
        }
    }

    /* ---- 屏幕文字（只显示，不叠到上报图）---- */
    if (smoke_now)
    {
        float elapsed = s.smoke_start_ms
            ? (ctx->timestamp_ms - s.smoke_start_ms) / 1000.0f : 0.0f;
        char buf[64];
        snprintf(buf, sizeof(buf), "SMOKE %.1fs / %.1fs", elapsed, confirm_sec);
        draw_text(ctx, buf, cv::Point(20, 36),
                  cv::Scalar(0, 0, 255), 0.7, 2, DrawCommand::DISPLAY);
    }
    else
    {
        const char *status    = s.alarm_sent ? "COOLING" : "CLEAR";
        cv::Scalar  color     = s.alarm_sent ? cv::Scalar(0, 165, 255)
                                             : cv::Scalar(0, 255, 0);
        draw_text(ctx, status, cv::Point(20, 36), color, 0.7, 2, DrawCommand::DISPLAY);
    }

    /* ---- 把 smoke 框标红（屏幕 + 上报图都改色）---- */
    if (ctx->results)
    {
        for (auto &r : *ctx->results)
            if (r.label == "smoke")
                r.box_color = cv::Scalar(0, 0, 255);
    }

    /* ---- 画 ROI 边框 ---- */
    if (ctx->roi && ctx->roi->size() >= 3)
    {
        cv::Scalar roi_color = smoke_now ? cv::Scalar(0, 0, 255) : cv::Scalar(0, 255, 255);
        for (size_t i = 0; i < ctx->roi->size(); ++i)
        {
            const cv::Point &p1 = (*ctx->roi)[i];
            const cv::Point &p2 = (*ctx->roi)[(i + 1) % ctx->roi->size()];
            draw_line(ctx, p1, p2, roi_color, 2);
        }
    }
}
```

### 第三步：自注册（同一文件末尾加一行）

```cpp
REGISTER_LOGIC("logic_smoke_detect", logic_smoke_detect);
```

无需改动 `channel_logic.cpp`。新增文件后重新 configure 让 CMake 收录：
`cmake -S . -B build && cmake --build build`。

### 第四步：配置文件

```json
{
  "global": {
    "model_path": "assets/yolov8n.rknn",
    "model_type": "yolov8_det",
    "label_path": "assets/labels.txt"
  },
  "channels": [
    {
      "id": 0,
      "stream": { "url": "rtsp://192.168.1.100:554/stream" },
      "logic": "logic_smoke_detect",
      "smoke_confirm_sec": 3.0,
      "smoke_cooldown_sec": 60,
      "roi": [[100, 100], [540, 100], [540, 480], [100, 480]]
    }
  ]
}
```

修改 `smoke_confirm_sec` 或 `smoke_cooldown_sec` 后**无需重启**，约 4 秒内热重载生效。

---

## 5. 附录：常见问题

**Q：怎么知道当前有哪些可用的目标类别？**

查看 `assets/labels.txt`，每行一个类别名，行号对应 `class_id`。`r.label` 即为文本名（如 `"person"`）。

---

**Q：`track_id` 什么时候才有效？**

`r.track_id >= 0` 表示该目标已被 SORT 跟踪器确认（连续命中 `tracker_min_hits` 帧，默认 3 帧）。`-1` 表示新出现或漏检中的不稳定目标。可在配置里关掉跟踪（`"tracker_enable": 0`），则所有目标的 `track_id` 均为 `-1`。

---

**Q：全局逻辑可以往屏幕上画东西吗？**

不能直接画。`draw_cmds` 属于各通道的 channel_logic（在 `dispatch_worker` 线程写入），全局逻辑在独立线程，直接写会有竞态。全局逻辑只能：① 调用 `alarm_uploader_enqueue` 上报带叠加的图片；② 通过 `get_channel_snapshot` 取到帧后用 OpenCV 直接在克隆图上画，再上报。屏幕显示由各通道自己的 channel_logic 负责。

---

**Q：`ctx->frame` 坐标系是什么？**

所有坐标（检测框 `r.box`、ROI 多边形、`draw_cmds`）统一在模型输入尺寸（默认 640×640）的坐标系下。可用 `algorithm_get_input_w()` / `algorithm_get_input_h()` 查询实际尺寸。

---

**Q：通道逻辑里能访问其他通道的数据吗？**

可以，调用 `ctx->get_channel_snapshot(other_chnId)` 即可，接口与全局逻辑的 `gctx->get_channel_snapshot()` 完全相同，保证 frame 与 results 同帧原子读出。

---

**Q：告警上报了但收不到数据？**

1. server 告警看本地发件箱积压：`ls <App>/alarm_store/*.json`（dify 才看 `redis-cli llen dify_queue`）
2. 确认 `unified_upload` Python 服务正在运行（`journalctl -u unified_upload -f`）
3. 检查 `dist/services/upload/config.yaml` 中的 `server.url` 是否可访问
4. 确认 Redis 服务已启动（`redis-cli ping` 返回 `PONG`）

---

**Q：逻辑函数里抛异常或段错误会怎样？**

段错误会导致进程崩溃（C++ 无法捕获）。建议：① 开头检查所有指针（`if (!ctx) return;`）；② 访问 results 前检查 `if (!ctx->results || ctx->results->empty())`；③ 克隆帧前检查 `if (ctx->frame && !ctx->frame->empty())`。框架本身不会因逻辑错误崩溃，但你的代码里的空指针访问会。

---

**Q：如何调试帧与检测结果是否对齐？**

在 `config.json` 中开启 `"debug_display": true`，`dispatch_worker` 会每 2 秒打印一行：

```
[FrameSync][ch00] result_seq=1234 input_seq=1238 lag=4 results=3
```

`lag` 是结果相对当前帧的延迟帧数，正常情况下为 1~3 帧（推理延迟）。数字过大说明推理队列积压。
