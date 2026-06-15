# 分析器模块 (analyzer/)

> 系统的核心调度层：协调 RGA 格式转换、NPU 推理入队、目标跟踪、通道业务逻辑、全局逻辑和异步显示。

---

## 文件清单

| 文件 | 职责 |
|------|------|
| `analyzer.h / analyzer.cpp` | 分析器生命周期、`videoOutHandle` 帧处理总入口、ROI 加载、通道在线/离线通知 |
| `analyzer_internal.h` | 各 .cpp 共享的内部状态（显示队列、推理标志等），不对外暴露 |
| `frame_inlet.cpp` | FPS 节流决策（是否送推理）、首帧黑图兜底、帧间隔 `dt_ms` 计算 |
| `rga_convert.cpp` | RGA 硬件格式转换：NV12/NV21 → BGR，DMA-BUF 零拷贝导入 |
| `frame_pipeline.h` | RGA 转换与显示提交的公共接口声明；实现分散在 `rga_convert.cpp` 与 `display_render.cpp` |
| `algoProcess.h / algoProcess.cpp` | NPU 推理队列管理：多路任务队列、worker 线程池、帧-结果配对缓冲区 |
| `algo_engine.cpp / algo_internal.h` | 推理引擎内部实现：模型工厂、RKNN 上下文管理 |
| `tracker.h / tracker.cpp` | SORT 目标跟踪：IoU 匹配 + 卡尔曼滤波，填入 `AlgoResult::track_id` |
| `channel_pipeline.cpp` | 单通道处理：调用 tracker → 调用 channel_logic → 写 draw_cmds |
| `result_dispatch.cpp` | `dispatch_worker_thread`：取推理结果 → 调 channel_pipeline → 触发显示入队 |
| `display_pipeline.cpp` | `display_worker_thread`：从显示队列取帧 → RGA 缩放 → render_overlays → framebuffer |
| `display_render.cpp` | tile 布局计算 + `commitImgtoDispBufMap`：RGA 缩放 → 调 `render_overlays`（实现在 `src/player/display.cpp`）→ 写 framebuffer |

> **注意**：业务逻辑文件（`channel_logic`、`global_logic`、`logic_tools`）位于 `src/logic/`，不在本目录。

---

## 整体数据流

```
GStreamer appsink 回调（解码线程）
    │
    ▼
videoOutHandle()               ← 帧处理总入口（analyzer.cpp）
    │
    ├─ FPS 节流检查（基于 steady_clock）
    │    └─ 未到推理时间 → 只送显示队列，跳过 RGA+推理
    │
    ├─ RGA 转换（rga_convert.cpp）
    │    └─ NV12/NV21 → BGR 640×640，DMA-BUF 零拷贝
    │
    ├─ process_channel_results()    ← 取推理结果 + 调用 logic
    │    ├─ algorithm_take_results()   从 NPU worker 取配对帧和结果
    │    ├─ Tracker::update()          SORT 跟踪，填 track_id
    │    └─ channel_logic_get()() → ChannelLogicFunc(ctx)
    │         └─ 写 draw_cmds，可上报告警
    │
    ├─ 异步显示入队（DispQueue, 最新解码帧）
    │    └─ display_worker 线程：RGA 缩放 → render_overlays(读共享 last_results + 速度外推) → framebuffer
    │       (屏幕实时预览, disp_fps 在此线程内权威更新; 上报用的匹配数据走 logic 的 ctx->frame)
    │
    └─ algorithm_process_mat()     ← NPU 推理入队
         └─ worker 线程：model->infer() → 写结果缓冲区
```

---

## algoProcess — 推理引擎

### 多路并发设计

每个通道独立一个任务队列 + 一组 NPU worker 线程（线程数由 `channel_threads` 配置），各通道互不阻塞：

```
通道 0 队列 → worker_0_0, worker_0_1  （threads=2）
通道 1 队列 → worker_1_0              （threads=1）
通道 N 队列 → worker_N_0 ...
```

### NPU 核心分配

- `npu_core=-1`（自动）：按通道索引轮询分配 RKNN_NPU_CORE_0/1/2
- `npu_core=0/1/2`：绑定指定核心（高负载通道可专用一个核心）
- `npu_cores=3`（全局）：控制同时使用的 RKNN 上下文数量

### 推理结果提取

推理结果与输入帧严格配对存储，通过以下函数原子取出：

```cpp
// 带配对帧的版本（推荐）
bool algorithm_take_results(int chnId,
                             vector<AlgoResult> &out,
                             cv::Mat &out_frame);
// out_frame：产出这批 results 时的 YOLO 输入帧（BGR，640×640）
// 返回 false：没有新结果，out 和 out_frame 被清空
```

### 热重载接口

```cpp
// 运行时重载指定通道的模型（耗时，应在锁外调用）
void algorithm_reload_channel_model(int chnId, const ChannelConfig &new_cfg);

// 更新检测阈值（无需重载模型）
void algorithm_update_thresh(int chnId, float obj_thresh, float nms_thresh);

// 更新类别白名单
void algorithm_update_detect_classes(int chnId, const vector<string> &class_names);

// 获取当前推理帧率（atomic，无锁）
float algorithm_get_infer_fps(int chnId);
```

---

## 通道业务逻辑（channel_logic）

### 设计原则

- 每帧在**解码回调线程**中同步调用，不额外开线程
- 通过 `ctx->state` 跨帧保存状态
- 输出为 `draw_cmds`（绘制指令列表），由 `render_overlays` 渲染到画面
- 坐标系统一为**模型输入尺寸**（如 640×640），`render_overlays` 自动缩放到屏幕分辨率

### 帧一致性契约

`ChannelContext` 中以下四个字段来自**同一帧**，不会错位：

| 字段 | 说明 |
|------|------|
| `frame` | BGR 图像，产出 `results` 的那张 NPU 输入帧 |
| `results` | 该帧的检测结果 |
| `frame_id` | 帧序号（推理通道=NPU 帧序号） |
| `timestamp_ms` | 帧时间戳（毫秒） |

### ChannelContext 字段速查

```cpp
struct ChannelContext {
    int             chnId;            // 通道号
    const cv::Mat  *frame;            // BGR，640×640，绝不为空（首帧 RGA 失败时有兜底）
    int64_t         frame_id;         // 帧序号
    uint64_t        timestamp_ms;     // 帧时间戳（ms）
    float           dt_ms;            // 与上次 logic 调用的时间差（首次=0）
    vector<AlgoResult> *results;      // 当前帧检测结果（可修改 box_color）
    const ChannelConfig *config;      // 通道配置（只读）
    const vector<cv::Point> *roi;     // ROI 多边形（模型坐标系，已缩放）；无 ROI 时为空
    vector<DrawCommand> *draw_cmds;   // 绘制指令（logic 往这里写）
    shared_ptr<void> *state;          // 跨帧持久化状态（自行 cast）
    int             infer_enabled;    // 该通道是否启用了推理（0/1）
    float           infer_fps;        // 本通道推理帧率（实时）
    float           disp_fps;        // 本通道显示帧率（实时）
    ChannelLogicFunc fn;              // 当前逻辑函数指针（由分发层注入）

    // 便捷方法（详见 channel_logic.h）
    int    has_target(const char *label) const;
    int    has_target_in_roi(const char *label) const;
    int    is_in_roi(const cv::Rect &box) const;
    int    target_count(const char *label) const;
    cv::Mat snapshot() const;  // frame->clone()，可直接上报
    ChannelSnapshot get_channel_snapshot(int chnId) const;  // 跨通道取数
};
```

### 内置逻辑一览

| 逻辑名称 | 功能描述 |
|----------|----------|
| `logic_default` | 不做额外处理，直接显示检测结果 |
| `logic_custom` | ROI 过滤演示（框内/框外着色）+性能信息显示 |
| `logic_server` | 有检测结果时上报到业务服务器队列 |
| `logic_dify` | 有检测结果时触发 Dify AI 分析流水线 |
| `logic_hook` | 安全圈越界检测：监控目标是否离开指定圆形区域 |
| `logic_roll` | 物料占用率检测：基于 HSV 颜色分析 ROI 区域占用比 |
| `logic_person_alarm` | 人员入侵检测：ROI 区域内有人时触发报警，状态供 `global_example` 读取 |
| `logic_cross_camera` | 跨摄像头参数获取演示，展示 `app_ctrl_*` 的完整用法 |
| `logic_dify_person_verify` | 人员 Dify 去重核验：按 track_id 去重，仅新出现的人员上报 |
| `logic_roi` | ROI/坐标可视化调试：标注 ROI 顶点与检测框中心坐标 |
| `logic_multi_roi` | 多 ROI 区域示例：逐区域计数、按区域染色 |
| `logic_wafer` | 晶圆盘擦拭工序：轨迹/覆盖率/动作识别/合规结算 |
| `logic_wafer_sop` | 晶圆清洗 SOP 合规检测：花篮进槽顺序/朝向/抖动 |

### 绘制辅助函数

所有坐标基于模型输入尺寸（如 640×640），渲染时自动缩放：

```cpp
void draw_rect(ChannelContext *ctx, const cv::Rect &rect,
               cv::Scalar color = {0,255,0}, int thickness = 2);

void draw_circle(ChannelContext *ctx, cv::Point center, int radius,
                 cv::Scalar color = {0,255,0}, int thickness = 2);

void draw_line(ChannelContext *ctx, cv::Point pt1, cv::Point pt2,
               cv::Scalar color = {0,255,0}, int thickness = 2);

void draw_text(ChannelContext *ctx, const string &text, cv::Point pos,
               cv::Scalar color = {255,255,255},
               double font_scale = 0.6, int thickness = 1);
```

---

## 全局逻辑（global_logic）

### 与 channel_logic 的核心区别

| | channel_logic | global_logic |
|--|---------------|--------------|
| 绑定关系 | 一对一（每路摄像头一个逻辑） | 不绑定，可同时读多路通道 |
| 执行时机 | 每帧解码回调中同步执行 | 独立线程按轮询间隔执行 |
| 典型用途 | 单通道目标过滤、ROI 检测、告警上报 | 跨摄像头目标关联、多区域联动告警 |
| 跨帧状态 | `ctx->state`（通道绑定） | `gctx->state`（实例绑定） |

### GlobalContext 字段速查

```cpp
struct GlobalContext {
    const GlobalLogicConfig *config;         // 实例配置（只读）
    uint64_t timestamp_ms;                   // 当前时间戳
    int64_t  tick_id;                        // 轮询计数
    const vector<int> *channel_ids;          // 监控的通道号列表
    const vector<vector<AlgoResult>> *all_results;  // 各通道检测结果快照
    bool    has_new_infer;                   // 本轮是否有新推理
    int     latest_infer_channel;            // 最新推理来自的通道
    shared_ptr<void> *state;                 // 跨帧持久化状态

    // 便捷方法
    const vector<AlgoResult> &get_channel_results(int chnId) const;
    cv::Mat   get_channel_frame(int chnId) const;
    bool      get_channel_snapshot(int chnId, ChannelSnapshot &out) const;
    template<typename T> shared_ptr<T> get_channel_state(int chnId) const;
    template<typename T> shared_ptr<T> get_channel_state_fresh(int chnId, uint64_t max_age_ms) const;
    float     get_channel_infer_fps(int chnId) const;
    bool      channel_has_target(int chnId, const string &label, int ms) const;
    template<typename Fn> void for_each_channel(Fn &&fn) const;
};
```

---

## 目标跟踪（Tracker）

基于 SORT 算法（IoU 匹配 + 卡尔曼滤波），在 `analyzer.cpp` 中统一管理，不在 logic 层调用：

- 每路通道独立一个 `Tracker` 实例
- 推理产出新结果后立即调用 `tracker->update(results)`，填入 `track_id`
- 通过配置控制：`tracker_enable`、`tracker_iou_thresh`、`tracker_max_miss`、`tracker_min_hits`

---

## ROI 配置（支持一个通道配多个区域）

ROI 从 `assets/roi_zones.json` 加载，key=通道序号(0,1,2…，按位置排序，非 channel_id)。

**新格式（多区域，推荐）** —— 每个区域可取一个名字，逻辑里按名/按序号调用：

```json
{
  "0": {
    "zones": [
      { "name": "entrance", "polygon": [[0.1, 0.2], [0.4, 0.2], [0.4, 0.7], [0.1, 0.7]] },
      { "name": "exit",     "polygon": [[0.6, 0.2], [0.9, 0.2], [0.9, 0.7], [0.6, 0.7]] }
    ]
  }
}
```

**旧格式（单区域，向后兼容）** —— 视为一个无名区域：

```json
{ "0": { "polygon": [[100, 100], [540, 100], [540, 380], [100, 380]] } }
```

- 坐标格式自动识别：**归一化(0~1)** 与视频源/分辨率解耦（网页默认输出，推荐）；**整数像素**为旧格式，运行时按 源→模型 缩放。
- 程序统一把各区域换算到模型输入坐标系（如 640×640）。
- 逻辑里访问：`ctx->rois`（全部区域）、`ctx->roi_by_name("entrance")`、`ctx->roi_polygon_at(i)`、
  `ctx->target_count_in_roi("person", i)` 等；`ctx->roi` 仍指向第一个区域（兼容老逻辑）。
- 显示画面会把**所有**区域都画成黄色多边形。
- 网页里：一个 ROI 节点(=一个通道)即可画**多个**命名区域——在 ROI 绘制弹窗右侧「区域列表」里「＋新增区域」、逐个画并各自命名。

---

## 二次开发指南

### 添加自定义通道逻辑（最常见需求）

**步骤一：** 新建 `src/logic/logic_my_alarm.cpp`（顶部 `#include "logic_common.h"`）实现逻辑函数：

```cpp
static void logic_my_alarm(ChannelContext *ctx) {
    if (!ctx || !ctx->results) return;

    // 跨帧状态（首次调用时初始化）
    struct MyState {
        int alarm_count = 0;
        uint64_t last_alarm_ms = 0;
    };
    if (!*ctx->state)
        *ctx->state = std::make_shared<MyState>();
    auto &s = *std::static_pointer_cast<MyState>(*ctx->state);

    // 检测 ROI 内的人（ctx->results / ctx->roi / ctx->frame 都是指针，先解引用）
    int has_roi = (ctx->roi && ctx->roi->size() >= 3);
    bool found = false;
    for (auto &r : *ctx->results) {
        if (r.label != "person") continue;

        // 检查是否在 ROI 内
        if (has_roi &&
            cv::pointPolygonTest(*ctx->roi, r.box_center(), false) < 0)
            continue;

        found = true;
        r.box_color = cv::Scalar(0, 0, 255);  // 红色框标注

        // 上报告警（冷却 5 秒）
        if (ctx->frame && ctx->timestamp_ms - s.last_alarm_ms > 5000) {
            alarm_uploader_enqueue(*ctx->frame, *ctx->frame,
                                   ctx->chnId, "my_alarm");
            s.last_alarm_ms = ctx->timestamp_ms;
            s.alarm_count++;
        }
    }

    // 绘制状态文字
    char info[64];
    snprintf(info, sizeof(info), "%s (count=%d)",
             found ? "ALARM" : "CLEAR", s.alarm_count);
    draw_text(ctx, info, {20, 30},
              found ? cv::Scalar(0,0,255) : cv::Scalar(0,255,0), 0.7, 2);
}
```

**步骤二：** 在同一文件末尾自注册（无需改动 `channel_logic.cpp`）：

```cpp
REGISTER_LOGIC("logic_my_alarm", logic_my_alarm);  // ← main() 之前自动登记到分发表
```

> `src/logic` 下的 `.cpp` 由 CMake 自动收集；新增/删除文件后重新 configure（`cmake -S . -B build`）即可。

**步骤三：** 在 `config.json` 中为对应通道指定：

```json
{
  "channels": [
    { "id": 0, "logic": "logic_my_alarm", ... }
  ]
}
```

支持热重载：修改 `config.json` 后无需重启，程序自动切换逻辑并重置跨帧状态。

---

### 添加全局逻辑（多路联动）

**步骤一：** 在 `global_logic.cpp` 中实现：

```cpp
static void global_my_logic(GlobalContext *gctx) {
    if (!gctx) return;

    // 统计有人的通道数量
    int alarm_channels = 0;
    gctx->for_each_channel([&](int chnId, int) {
        if (gctx->channel_has_target(chnId, "person", 300))
            alarm_channels++;
    });

    if (alarm_channels >= 2) {
        // 获取第一个报警通道的帧快照
        gctx->for_each_channel([&](int chnId, int) {
            ChannelSnapshot snap;
            if (gctx->get_channel_snapshot(chnId, snap) && snap.has_results) {
                alarm_uploader_enqueue(snap.frame, snap.frame,
                                       chnId, "multi_zone");
            }
        });
    }
}
```

**步骤二：** 在 `global_logic_register()` 中注册：

```cpp
g_logic_map["global_my_logic"] = global_my_logic;
```

**步骤三：** 在 `config.json` 中配置：

```json
{
  "global": {
    "global_logics": [
      {
        "enable": true,
        "logic": "global_my_logic",
        "channels": [],
        "poll_interval_ms": 100
      }
    ]
  }
}
```

---

### 读取其他通道的业务状态

若需要在 global_logic 中读取某个通道 `logic_hook` 的状态：

```cpp
// logic_hook.h 中定义了 HookState
auto hook = gctx->get_channel_state_fresh<HookState>(0, 500);
if (hook && hook->alarm_sent_latch) {
    // 通道 0 的 hook logic 在 500ms 内发送过报警
}
```

若不确定状态是否过期，使用 `get_channel_state_fresh<T>(chnId, max_age_ms)`；若不关心新鲜度，使用 `get_channel_state<T>(chnId)`。

---

### 异步显示队列调优

显示线程与解码线程完全解耦，采用**覆盖策略**（新帧覆盖未被消费的旧帧）。如果显示线程处理能力不足导致画面掉帧，可通过降低 `max_fps` 减轻 RGA 压力：

```json
{
  "global": { "max_fps": 15 }
}
```
