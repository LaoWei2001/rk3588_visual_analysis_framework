# ChannelContext API 参考

逻辑函数签名固定：`static void logic_xxx(ChannelContext *ctx)`。
所有数据从 `ctx` 来，都是**本通道、本帧**的。坐标系 = 模型输入尺寸（通常 640×640）。
权威定义见 `vision_analysis/src/logic/core/channel_logic.h`。

## ctx 字段

| 字段 | 类型 | 含义 |
|------|------|------|
| `ctx->chnId` | `int` | 系统唯一的通道 ID（始终等于 `config.channels[].id`） |
| `ctx->frame` | `const cv::Mat *` | 当前帧（BGR，**模型输入尺寸**）。**可能为空，先判空** |
| `ctx->source_frame()` | `const cv::Mat *` | 按需取得当前解码源帧转换后的原分辨率 BGR 图；传统 CV 同步通道可用，失败或异步推理路径返回 `nullptr` |
| `ctx->src_width` / `ctx->src_height` | `int` | 原始视频源分辨率；首帧解码前可能为 0。它们不同于模型输入尺寸 |
| `ctx->frame_id` | `int64_t` | 单调递增帧号 |
| `ctx->timestamp_ms` | `uint64_t` | 单调钟（毫秒）——只用于计时/限频，**不是日历时间** |
| `ctx->unix_ms` | `uint64_t` | 墙上时钟 Unix epoch 毫秒（真实时间，RTSP/USB/文件 三源统一）；配 `time_hms()`/`time_str()`/`datetime()` |
| `ctx->dt_ms` | `float` | 距上一帧毫秒数（积分用） |
| `ctx->results` | `std::vector<AlgoResult> *` | 本帧检测结果（模型坐标系）。可能为空 |
| `ctx->config` | `const ChannelConfig *` | 本通道公共配置（只读），例如 stream、模型和上报策略；模块专有参数使用下面的 `param_*()` |
| `ctx->logic_parameters` | `const LogicParameterSet *` | 框架内部的类型化参数表；logic 通常不直接操作，使用 `param_*()` |
| `ctx->roi` | `const std::vector<cv::Point> *` | ROI 多边形顶点（**已缩放到模型坐标系**）；无 ROI 时为空 |
| `ctx->rois` | `const std::vector<RoiZone> *` | 本通道全部命名 ROI；无 ROI 时指向空列表或为空，使用前应检查 |
| `ctx->draw_cmds` | `std::vector<DrawCommand> *` | 绘制指令输出（用 `draw_*` 函数往里加，别直接操作） |
| `ctx->canvas` / `ctx->show_canvas` | 框架内部画布指针 | 通常不要直接操作；需要改显示底图时调用 `display_canvas()` |
| `ctx->state` | `std::shared_ptr<void> *` | 本通道**跨帧**持久状态（见下方模式） |
| `ctx->infer_enabled` | `int` | 本通道是否在推理 |
| `ctx->infer_fps` / `ctx->disp_fps` | `float` | 实时帧率 |

### 原始分辨率帧（传统 CV）

`ctx->frame` 继续作为模型坐标系基准；只有算法确实依赖高清细节时才调用 `source_frame()`：

```cpp
const cv::Mat *source = ctx->source_frame();
if (!source || source->empty())
    return;

cv::Mat gray;
cv::cvtColor(*source, gray, cv::COLOR_BGR2GRAY);
```

第一次调用会把当前解码源帧按原始宽高转换为 BGR，同一帧内再次调用复用结果；完全不调用则没有额外高清转换。返回的 `cv::Mat` 只在本次 logic/action 回调期间有效，不能缓存到 `ctx->state`、跨帧持有或交给异步线程。

该入口目前保证用于同步传统 CV 路径。推理通道的 logic 在 NPU 完成后异步执行，届时原始 GStreamer 缓冲已经归还，因此 `source_frame()` 返回 `nullptr`；这类逻辑仍使用与检测结果严格同帧的 `ctx->frame`。

`source_frame()` 使用源像素坐标，而 ROI、检测框、`draw_*` 和 `ctx->frame` 使用模型输入坐标。需要互相映射时按宽高比例换算，不能直接混用坐标。

## 模块专有参数

参数在当前模块 `logic.json.parameters.properties` 声明，启动/热重载时由框架校验并补齐默认值。逐帧直接读取类型化快照，不解析 JSON：

```cpp
float       seconds = ctx->param_float("dwell_sec");
int64_t     count   = ctx->param_int("max_count");
bool        enabled = ctx->param_bool("overlay_enabled");
std::string mode    = ctx->param_string("mode");
std::string rules   = ctx->param_json("rules");
bool        exists  = ctx->has_param("dwell_sec");
```

完整的 Schema、Web 和热重载流程见 [adding-config-parameter.md](adding-config-parameter.md)。视频源分辨率、FPS、stream URL 等公共信息不是模块参数，继续从 `ctx`/`ctx->config` 获取。

## 便捷查询（本通道；整帧查询是 ctx 方法，ROI 查询是自由函数）

```cpp
int  ctx->has_target(const char *label);            // 本帧有无该类别目标
int  roi_has_target(const ChannelContext *ctx, const char *label, int idx);   // ROI 内有无该类别（idx=ROI_ALL 任一区域；无 ROI=整帧）
int  roi_count_target(const ChannelContext *ctx, const char *label, int idx); // ROI 内该类别数量（ROI_ALL=所有区域并集）
int  roi_contains(const ChannelContext *ctx, const cv::Rect &box, int idx);   // 框中心是否在 ROI 内（idx=ROI_ALL 且未画 ROI 时按整帧处理，返回 1）
int  ctx->target_count(const char *label);          // 该类别目标数量
std::string ctx->time_hms();                        // 本帧真实时间 "HH:MM:SS"(三源统一); 原始值 ctx->unix_ms = epoch 毫秒
FrameTime   ctx->datetime();                        // 拆成 .year/.month/.day/.hour/.minute/.second/.millis (各是 int)
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
| `roi_contains(ctx, box, idx)` | 框中心是否在 ROI 内（`idx=ROI_ALL` 任一区域并集；`>=0` 仅该区域；没画区域=整帧） |
| `roi_has_target(ctx, label, idx)` | ROI 内是否有某类别目标（`idx` 取值同上） |
| `roi_count_target(ctx, label, idx)` | ROI 内某类别目标数量（`ROI_ALL` 并集、重叠不重复计；`>=0` 仅该区域） |
| `roi_find(ctx, "entrance")` | 名字 → 区域序号（找不到=`ROI_NONE`）；按名字查时传给上面三个函数的 `idx` |
| `ctx->roi_index_of(box)` | 框中心落在第几个区域（取首个命中；都不在/无区域 → `ROI_NONE`，当前值为 -2） |
| `ChannelContext::point_box_in_poly(poly, box)`（静态） | 框中心是否在给定多边形内（多边形 <3 点视为"全屏"=1） |

> 单区域、多区域用同一套 C 风格自由函数 `roi_contains` / `roi_has_target` / `roi_count_target`（`idx` 传 `ROI_ALL` = 所有区域）。ROI 进入告警模式见 `examples/roi-alarm-pattern.md`，复杂的按名区域流程见 `examples/logic_path_sop.md`。

### 为什么有些 ROI API 在结构体内，有些在结构体外

结构体内的是 C++ 成员函数，例如：

```cpp
ctx->roi_count();
ctx->roi_polygon_at(0);
ctx->roi_index_of(box);
```

调用时编译器会隐式传入 `this=ctx`，适合表达“从这个上下文取数据”或“让这个上下文执行基础查询”。不能对空指针调用成员函数，必须先保证 `ctx != nullptr`。

结构体外的是自由函数，例如：

```cpp
roi_contains(ctx, box, ROI_ALL);
roi_has_target(ctx, "person", ROI_ALL);
```

它们显式接收 `ctx`，把“区域选择规则 + 目标遍历 + 空上下文保护”组合成统一的 C 风格调用形式。这里的“C 风格”只是调用形式简单，并不表示这些带 C++ 类型的函数具有 C ABI。自由函数可以安全判断 `ctx == nullptr`，也方便用同一个 `idx` 参数处理 `ROI_ALL`、具体区域和 `ROI_NONE`。

二者在能力和性能上没有本质高低：这些自由函数技术上也能改成成员函数，成员函数也能再包一层自由函数。当前划分主要是 API 组织和历史兼容；真正有区别的是调用语法、是否隐式拥有 `this`，以及空指针能否在函数内部处理。

## 跨通道安全取数（需要别的通道画面/结果时）

```cpp
ChannelSnapshot s = ctx->get_channel_snapshot(2);   // 原子取通道2的 frame+results+fps（深拷贝快照）
if (!s.frame.empty() && s.result_age_ms < 500) {    // 新鲜度自检
    for (auto &r : s.results) { /* r.box / r.label / r.score ... */ }
}
std::string name = ctx->get_channel_logic_name(2);  // 通道2跑的是哪个 logic
int yes = ctx->channel_has_logic(2, "logic_xxx");
```
> 本通道当帧数据直接用 `ctx->frame` / `ctx->results` 即可（已保证同帧）。跨通道**必须**用 `get_channel_snapshot`，不要直接摸别的通道状态。

## AlgoResult（`ctx->results` 的元素）

定义见 `vision_analysis/src/analyzer/algoProcess.h`。

| 字段 | 含义 |
|------|------|
| `r.box` | `cv::Rect` 检测框（模型坐标系） |
| `r.label` | `std::string` 类别名（**与 labels.txt 完全一致**） |
| `r.class_id` | `int` 类别下标 |
| `r.score` | `float` 置信度 |
| `r.track_id` | `int` 跟踪 ID（跟踪器赋值，跨帧稳定；<0 表示未确认） |
| `r.model_id` | `std::string` 来源模型 ID（对应 `ctx->config->models[].id`） |
| `r.model_type` | `std::string` 来源模型类型 |
| `r.model_index` | `int` 来源模型在本次有效模型列表中的序号 |
| `r.box_color` | `cv::Scalar` 设它可改这个框的显示颜色（默认 -1,-1,-1） |
| `r.keypoints` | `vector<Point2f>` 姿态点（pose 模型） |
| `r.boxMask` | `cv::Mat` 分割掩码（seg 模型） |

`model_id/model_type/model_index` 由多模型合并路径填写。业务若要枚举当前通道配置的全部模型（包括本帧没有产出结果的模型），应读取只读的 `ctx->config->models`。

便捷方法：`r.box_center()` → 框中心 `cv::Point`；`r.box_contains(pt)`；`r.dist_sq_to(pt)` → 中心到点距离平方（省 sqrt，适合阈值比较）。

## 绘制（坐标都用模型坐标系）

```cpp
void draw_rect (ctx, const cv::Rect &rect, color=绿, thickness=2, alpha=1.0, target=ALL);
void draw_circle(ctx, const cv::Point &center, int radius, color=绿, thickness=2, alpha=1.0, target=ALL);
void draw_line (ctx, const cv::Point &p1, const cv::Point &p2, color=绿, thickness=2, target=ALL);
void draw_text (ctx, const char *text, const cv::Point &pos, color=白, font_scale=0.6, thickness=1, target=ALL);
void draw_polyline(ctx, const std::vector<cv::Point> &points, color=绿, thickness=2, double alpha=1.0, bool closed=false, target=ALL); // 一串点连成线; alpha<1 半透明; closed=true 首尾相连(画 ROI 多边形常用)
void draw_poly_filled(ctx, const std::vector<cv::Point> &points, color=绿, alpha=0.3, target=ALL); // 半透明填充多边形
```
颜色是 BGR：红 `cv::Scalar(0,0,255)`、绿 `cv::Scalar(0,255,0)`、黄 `cv::Scalar(0,255,255)`。

> **中文/UTF-8 文字**：`draw_text` 统一走 freetype 渲染，直接支持中文（无需 cv::putText）。是否有可用 CJK 字体可用 `text_overlay_available()`（`player/text_overlay.h`）查询，据此在中文/ASCII 之间取舍。

`target`（`DrawCommand::Target`）声明绘制命令属于哪些层：

- `DrawCommand::DISPLAY` — 实时显示层；
- `DrawCommand::IMAGE` — 告警图片专用层；
- `DrawCommand::VIDEO` — 事件视频专用层；
- `DrawCommand::MEDIA` — `IMAGE | VIDEO`；
- `DrawCommand::ALL` — `DISPLAY | IMAGE | VIDEO`（默认）。

当前 Web 的“与实时播放窗口画面一致”会让告警图片取 `DISPLAY|IMAGE`，事件视频取 `DISPLAY|VIDEO`，因此 `DISPLAY` 层也会被这种媒体模式复用；选择原始图片/视频时不绘制任何层。例：只想额外写入告警媒体而不出现在实时画面，用 `DrawCommand::MEDIA`。

### `ctx->draw_cmds` 的真实作用

`draw_cmds` 是框架为本次 logic 调用创建的“本帧绘制命令输出队列”。业务代码看似没有使用它，是因为所有 `draw_*` 辅助函数已经替你操作：

```cpp
void draw_text(ChannelContext *ctx, ...)
{
    DrawCommand cmd;
    cmd.type = DrawCommand::TEXT;
    // 填充文字、坐标、颜色、Target……
    ctx->draw_cmds->push_back(cmd);
}
```

完整生命周期是：

```text
channel_pipeline 为本帧创建 vector<DrawCommand>
  └─ ctx.draw_cmds 指向它
       └─ logic 调 draw_text/draw_rect 等追加命令
            ├─ logic 返回后 move 到该通道状态，供实时显示读取
            ├─ report_event 调用时复制当前命令，供 annotated.jpg 渲染
            └─ 录像帧入口复制通道最近命令，供 clip.mp4 渲染
```

它采用“记录命令、出口延迟渲染”，所以同一套逻辑坐标能分别适配实时 tile、模型尺寸截图和源分辨率录像。它不是跨帧状态，指针在本次 logic 返回后就失效；不要缓存它，也通常不要直接 `push_back`。只有需要直接修改实时显示底图像素时才使用 `ctx->display_canvas()`，那条路径与 DrawCommand 不同。

上报图片会在 `report_event()` 调用期间复制当前队列，因此希望进入本次截图的 `draw_*` 必须写在上报调用之前。完整媒体分层示例见 [logic_upload_teach.md](examples/logic_upload_teach.md)。

## 跨帧状态模式（计时 / 闩锁 / 去重必用）

`ctx->state` 的类型是 `std::shared_ptr<void>*`：它是“指向框架所持 `shared_ptr<void>` 对象的普通指针”，不是可以与 `void**` 互换的传统二级指针。`ctx->state` 访问外层指针，`*ctx->state` 才取得那一格智能指针。第一次用时创建状态对象，之后每帧取回同一份：

```cpp
#include <set>

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

- **函数是无状态的纯代码**：每个通道的 `ChannelConfig`、类型化模块参数、ROI 和运行状态都按唯一 `channel id` 分开保存，并通过不可变运行快照发布；共用结构体定义不等于共用同一份数据。
- **ctx 每帧栈上现搭、只指向本通道数据**：框架（`channel_pipeline.cpp` 的 `invoke_channel_logic`）每帧新建一个 `ChannelContext ctx;`，从同一代快照填入 `ctx->config`、`ctx->logic_parameters` 和 `ctx->rois`，再绑定本通道 results/state。所以同一个函数被 8 路调用，是“同一段代码 + 8 份独立上下文”。
- **跨帧状态 = 每通道一格 `ctx->state`**：每通道第一次用各自 `make_shared` 自己的状态对象，互不可见（见上一节模式）。
- **换逻辑 / 上下线会 reset**：通道 `logic` 名变化或上下线时，框架会 `logic_state.reset()`（`app_ctrl.cpp` / `analyzer.cpp`），新逻辑从干净状态起步，不会读到旧逻辑残留、也不会类型错配。
- **并发**：不同通道写的是不同下标的槽（本就不冲突），同通道两条处理路径用 `g_process_mtx[chnId]` 串行、写回用 `chn_mtx[chnId]`。

> **唯一会破坏隔离的写法是 `static` / 全局可变变量**——它整个进程只有一份、被所有通道共享。跨帧、每通道的东西一律放 `ctx->state`。
> 全局逻辑（`global_xxx`）的隔离（每实例一份 `gctx->state`）见 **`rk3588-global-logic`** skill。

## 头文件与自注册
每个逻辑位于 `src/logic/modules/logic_xxx/`，入口源码顶部包含 `#include "logic/core/logic_common.h"`。它汇总了 ChannelContext、算法结果、告警和绘制常用接口；使用 `std::set`、`std::unordered_map` 等未汇总类型时仍须显式包含对应标准头。
文件末尾写 `REGISTER_LOGIC(logic_xxx);` 自注册；函数名会自动成为外部 logic ID。同目录维护不含 `name` 的 `logic.json`。CMake 自动递归收集 `.cpp/.cc/.cxx`；删除整个模块目录即可删除该逻辑。
