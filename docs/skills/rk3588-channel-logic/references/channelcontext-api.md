# ChannelContext API 参考

逻辑函数签名固定：`static void logic_xxx(ChannelContext *ctx)`。
所有数据从 `ctx` 来，都是**本通道、本帧**的。坐标系 = 模型输入尺寸（通常 640×640）。
权威定义见 `rk3588_yolo/src/logic/channel_logic.h`。

## ctx 字段

| 字段 | 类型 | 含义 |
|------|------|------|
| `ctx->chnId` | `int` | 本通道号（上报、跨通道取数要用） |
| `ctx->frame` | `const cv::Mat *` | 当前帧（BGR，**模型输入尺寸**）。**可能为空，先判空** |
| `ctx->frame_id` | `int64_t` | 单调递增帧号 |
| `ctx->timestamp_ms` | `uint64_t` | 本帧时间戳（毫秒）——计时/限频用它 |
| `ctx->dt_ms` | `float` | 距上一帧毫秒数（积分用） |
| `ctx->results` | `std::vector<AlgoResult> *` | 本帧检测结果（模型坐标系）。可能为空 |
| `ctx->config` | `const ChannelConfig *` | 本通道配置（只读）。取参数、上报地址都从这里 |
| `ctx->roi` | `const std::vector<cv::Point> *` | ROI 多边形顶点（**已缩放到模型坐标系**）；无 ROI 时为空 |
| `ctx->draw_cmds` | `std::vector<DrawCommand> *` | 绘制指令输出（用 `draw_*` 函数往里加，别直接操作） |
| `ctx->state` | `std::shared_ptr<void> *` | 本通道**跨帧**持久状态（见下方模式） |
| `ctx->infer_enabled` | `int` | 本通道是否在推理 |
| `ctx->infer_fps` / `ctx->disp_fps` | `float` | 实时帧率 |

## ctx 便捷查询方法（本通道）

```cpp
int  ctx->has_target(const char *label);            // 本帧有无该类别目标
int  ctx->has_target_in_roi(const char *label);     // 有无该类别且中心落在 ROI 内（无 ROI 时等同 has_target）
int  ctx->is_in_roi(const cv::Rect &box);           // 某框中心是否在 ROI 内（无 ROI 返回 1）
int  ctx->target_count(const char *label);          // 该类别目标数量
cv::Mat ctx->snapshot();                            // 克隆当前帧
RenderParams ctx->render_params(int64_t age=0);     // 给 render_overlays 用（上报图叠框时）
```

## 多 ROI（一个通道画了多个命名区域时）

一个通道可在网页 ROI 节点里画**多个**区域并各自命名（如 `entrance`/`exit`）。`ctx->roi` 仍指向"第一个区域"（兼容老逻辑）；要按序号/名字访问全部区域，用下面这组方法（定义在 `channel_logic.h` 的 `ChannelContext`，多边形均为模型坐标系）：

| 方法 | 含义 |
|------|------|
| `ctx->rois` | `const std::vector<RoiZone>*`，本通道全部区域（`RoiZone{ std::string name; std::vector<cv::Point> polygon; }`） |
| `ctx->roi_count()` | 有效区域数量 |
| `ctx->roi_at(i)` / `roi_polygon_at(i)` / `roi_name_at(i)` | 按序号取 区域 / 多边形 / 名字（越界返回 nullptr 或 `""`） |
| `ctx->roi_by_name("entrance")` | 按名字取区域（找不到返回 nullptr） |
| `ctx->is_in_roi_idx(box, i)` | 框中心是否落在第 i 区域内 |
| `ctx->has_target_in_roi_idx(label, i)` | 第 i 区域内有无某类别目标 |
| `ctx->target_count_in_roi(label, i)` | 第 i 区域内某类别目标数量 |
| `ctx->target_count_in_roi_named(label, "entrance")` | 按名字统计某类别数量 |
| `ChannelContext::point_box_in_poly(poly, box)`（静态） | 框中心是否在给定多边形内（多边形 <3 点视为"全屏"=1） |

> 单区域逻辑继续用 `ctx->roi` / `has_target_in_roi` 即可。多区域完整范例见 `examples/logic_multi_roi.md`、`examples/logic_wafer_sop.md`（按名字把业务步骤绑到各区域）。

## 跨通道安全取数（需要别的通道画面/结果时）

```cpp
ChannelSnapshot s = ctx->get_channel_snapshot(2);   // 原子取通道2的 frame+results+fps（深拷贝快照）
if (!s.frame.empty() && s.result_age_ms < 500) {    // 新鲜度自检
    for (auto &r : s.results) { /* r.box / r.label / r.score ... */ }
}
std::string name = ctx->get_channel_logic_name(2);  // 通道2跑的是哪个 logic
int yes = ctx->channel_has_logic(2, "logic_person_alarm");
```
> 本通道当帧数据直接用 `ctx->frame` / `ctx->results` 即可（已保证同帧）。跨通道**必须**用 `get_channel_snapshot`，不要直接摸别的通道状态。

## AlgoResult（`ctx->results` 的元素）

定义见 `rk3588_yolo/src/analyzer/algoProcess.h`。

| 字段 | 含义 |
|------|------|
| `r.box` | `cv::Rect` 检测框（模型坐标系） |
| `r.label` | `std::string` 类别名（**与 labels.txt 完全一致**） |
| `r.class_id` | `int` 类别下标 |
| `r.score` | `float` 置信度 |
| `r.track_id` | `int` 跟踪 ID（跟踪器赋值，跨帧稳定；<0 表示未确认） |
| `r.box_color` | `cv::Scalar` 设它可改这个框的显示颜色（默认 -1,-1,-1） |
| `r.keypoints` | `vector<Point2f>` 姿态点（pose 模型） |
| `r.boxMask` | `cv::Mat` 分割掩码（seg 模型） |

便捷方法：`r.box_center()` → 框中心 `cv::Point`；`r.box_contains(pt)`；`r.dist_sq_to(pt)` → 中心到点距离平方（省 sqrt，适合阈值比较）。

## 绘制（坐标都用模型坐标系）

```cpp
void draw_rect (ctx, const cv::Rect &rect, color=绿, thickness=2, target=ALL);
void draw_circle(ctx, const cv::Point &center, int radius, color=绿, thickness=2, target=ALL);
void draw_line (ctx, const cv::Point &p1, const cv::Point &p2, color=绿, thickness=2, target=ALL);
void draw_text (ctx, const char *text, const cv::Point &pos, color=白, font_scale=0.6, thickness=1, target=ALL);
```
颜色是 BGR：红 `cv::Scalar(0,0,255)`、绿 `cv::Scalar(0,255,0)`、黄 `cv::Scalar(0,255,255)`。

`target`（`DrawCommand::Target`）控制画到哪：
- `DrawCommand::DISPLAY` — 只画到显示器/监看画面
- `DrawCommand::UPLOAD` — 只画到上报图
- `DrawCommand::ALL` — 都画（默认）

例：状态文字常只给显示 `draw_text(ctx, "OUT 1.2s", {410,330}, 红, 1, 2, DrawCommand::DISPLAY)`；命中框两边都要就用 `ALL`。

## 跨帧状态模式（计时 / 闩锁 / 去重必用）

`ctx->state` 是本通道独有的一格 `shared_ptr<void>`。第一次用时建，之后每帧取回同一份：

```cpp
struct MyState {
    uint64_t last_ms = 0;
    int      first   = 1;
    std::set<int> reported_ids;   // 例如按 track_id 去重
};

if (!*ctx->state) *ctx->state = std::make_shared<MyState>();
auto &s = *std::static_pointer_cast<MyState>(*ctx->state);
// 之后正常读写 s.xxx；它会跨帧保留，且与其它通道互不干扰
```
> **不要用 `static` 局部变量存跨帧状态**——那会被所有通道共享、互相串台。每通道独立状态只能放 `ctx->state`。

## 变量隔离：多通道共用同一个 logic 函数，为什么不串台

同一个 `logic_xxx` 会被所有通道复用，但**数据天生每通道独立**，靠"按通道号分槽 + ctx 每帧现搭"：

- **函数是无状态的纯代码**：通道的数据不在函数里，而在框架按 `chnId` 分好的槽里——`g_pCtrl->config.channels[chnId]`（本通道 `ChannelConfig`）、`g_pCtrl->channels_state[chnId]`（本通道 `logic_state` / `roi_for_logic` / `last_results`…）。`ChannelConfig`、`ChannelContext` 只是**类型/模板**；`channels[0]` 与 `channels[1]` 是**两块不同内存**，所以"共用结构体"= 共用定义，不是共用同一份数据。
- **ctx 每帧栈上现搭、只指向本通道那一槽**：框架（`channel_pipeline.cpp` 的 `invoke_channel_logic`）每帧新建一个 `ChannelContext ctx;`，把 `ctx->config`/`ctx->results`/`ctx->roi`/`ctx->state` 指向当前 `chnId` 的槽，再调用你。所以同一个函数被 8 路调用，是"同一段代码 + 8 个各自指向自家数据的 ctx"，ctx 本身不共享（用完即弃）。
- **跨帧状态 = 每通道一格 `ctx->state`**：每通道第一次用各自 `make_shared` 自己的状态对象，互不可见（见上一节模式）。
- **换逻辑 / 上下线会 reset**：通道 `logic` 名变化或上下线时，框架会 `logic_state.reset()`（`app_ctrl.cpp` / `analyzer.cpp`），新逻辑从干净状态起步，不会读到旧逻辑残留、也不会类型错配。
- **并发**：不同通道写的是不同下标的槽（本就不冲突），同通道两条处理路径用 `g_process_mtx[chnId]` 串行、写回用 `chn_mtx[chnId]`。

> **唯一会破坏隔离的写法是 `static` / 全局可变变量**——它整个进程只有一份、被所有通道共享。跨帧、每通道的东西一律放 `ctx->state`。
> 全局逻辑（`global_xxx`）的隔离（每实例一份 `gctx->state`）见 **`rk3588-global-logic`** skill。

## 头文件与自注册
每个逻辑是 `src/logic/` 下一个**独立的 `.cpp` 文件**，顶部只需 `#include "logic_common.h"` —— 它已汇总逻辑常用的全部头（`channel_logic.h` / `logic_tools.h` / `algoProcess.h` / `app_ctrl.h` / `alarm_uploader.h` / `text_overlay.h` / OpenCV 等），一般无需再加别的 include。
文件**末尾**写一行 `REGISTER_LOGIC("logic_xxx", logic_xxx);` 即自注册到分发表（在 `main()` 之前生效），**无需改动 `channel_logic.cpp` / `channel_logic_init()`**；删除该逻辑＝删除该文件。`src/logic` 下的 `.cpp` 由 CMake 自动收集编译。
