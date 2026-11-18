# ChannelContext 成员信息图

通道业务回调接收 `ChannelContext *ctx`。它把本通道的配置、当前帧、检测结果、
ROI 和业务状态放到同一个访问入口。以下按实际 C++ 名称列出数据成员和成员函数，
方便在编写 logic 时直接查找。

定义入口：[channel.h](channel.h)、[config_types.h](config_types.h)、[types.h](types.h)。
这里的 `ctx->config` 是当前通道的 `ChannelConfig`，不是整个程序的全局配置。

## 公开数据成员

图中的 `[]` 表示容器里的一个元素；`results` 和 `rois` 本身是指向容器的指针。
例如实际访问某个检测结果要写 `(*ctx->results)[i]`，或遍历 `*ctx->results`。

```text
ctx（ChannelContext *，本次通道回调的上下文）
│
├─ chnId : int                           通道 ID，对应 channels[].id
├─ src_width : int                       视频源原始宽度；首帧前可能为 0
├─ src_height : int                      视频源原始高度；首帧前可能为 0
├─ frame_id : int64_t                     当前业务帧号
├─ timestamp_ms : uint64_t                当前业务帧的分析时间，文件暂停不累计
├─ video_time_ms : int64_t                当前帧在文件中的毫秒位置；未知/RTSP/USB 为 -1
├─ real_time_ms : uint64_t               帧进入板端管线时的系统单调毫秒，供实际耗时判断
├─ unix_ms : uint64_t                     同一业务帧的 Unix 时间戳，毫秒
├─ dt_ms : float                         当前业务帧与上一业务帧的间隔，毫秒
│
├─ config : const ChannelConfig *        当前通道配置，只读
│  ├─ id : int                           配置中的通道 ID
│  ├─ enable : bool                      是否启用该通道
│  ├─ stream : StreamConfig              视频源配置，见下方展开
│  ├─ logic : string                     本通道绑定的业务模块名称
│  ├─ models[] : ChannelModelConfig      本通道的模型配置列表
│  │  ├─ id : string                     模型节点 ID，用于区分结果来源
│  │  ├─ enable : bool                   是否启用这个模型
│  │  ├─ model_type : string             模型类型，例如 yolov8_det
│  │  ├─ model_path : string             模型文件路径
│  │  ├─ label_path : string             标签文件路径
│  │  ├─ version : string                模型版本；未设置时为空
│  │  ├─ obj_thresh : float              置信度阈值，例如 0.9
│  │  ├─ nms_thresh : float              NMS 阈值
│  │  ├─ detect_classes[] : string       允许的检测类别；空表示不限类别
│  │  └─ npu_core : int                  -1 自动，0/1/2 固定 NPU 核心
│  ├─ inference_roi : InferenceRoiConfig 推理区域配置，见下方展开
│  ├─ roi_zones[] : RoiZoneConfig        配置文件中的业务 ROI
│  │  ├─ name : string                   区域名称
│  │  └─ polygon[] : pair<double,double> 归一化顶点，坐标范围 0～1
│  ├─ logic_parameters_json : string     业务参数 JSON；通常通过 param_*() 读取
│  ├─ threads : int                      通道推理工作线程数
│  ├─ max_fps : int                      推理和业务处理帧率上限
│  ├─ tracker_enable : int               跟踪开关
│  ├─ tracker_type : string              跟踪算法，例如 sort / bytetrack
│  ├─ tracker_iou_thresh : float         跟踪匹配 IoU 阈值
│  ├─ tracker_max_miss : int              允许连续丢失的帧数
│  ├─ tracker_min_hits : int              确认轨迹所需的命中帧数
│  ├─ bytetrack_low_thresh : float        ByteTrack 内部低分候选阈值
│  ├─ bytetrack_low_iou_thresh : float    ByteTrack 第二轮匹配 IoU 阈值
│  ├─ report_policy_json : string        事件上报策略 JSON
│  └─ event_video : EventVideoRuntimeConfig
│     ├─ enable : bool                   是否启用事件录像
│     ├─ pre_sec : float                 事件前录像时长，秒
│     ├─ post_sec : float                事件后录像时长，秒
│     ├─ fps : int                       事件录像帧率
│     └─ overlay : string                none / custom / all
│
├─ results : vector<AlgoResult> *        本帧模型结果列表
│  └─ [i] : AlgoResult                   一条模型结果，见下方完整展开
│     ├─ model_id : string               来源模型 ID，对应 models[].id
│     ├─ label : string                  目标类别名称，例如 person
│     ├─ score : float                   目标实际置信度，例如 0.95
│     ├─ box : cv::Rect                  目标框，使用 640×640 业务坐标
│     └─ track_id : int                  跟踪 ID；未分配时为 -1
│
├─ rois : const vector<RoiZone> *        已换算到业务坐标的 ROI，只读
│  └─ [i] : RoiZone
│     ├─ name : string                   区域名称
│     └─ polygon[] : cv::Point           640×640 业务坐标中的顶点
│
├─ state : shared_ptr<void> *           本逻辑实例的跨帧状态存储入口
│                                       通常使用 get_state<MyState>() 访问
├─ infer_enabled : int                  当前上下文是否启用了推理
├─ inference_valid : bool               本业务帧是否有有效的推理结果
├─ infer_fps : float                    推理完成帧率，不等于结果发布帧率
└─ disp_fps : float                     通道预览帧率
```

`threads`、`max_fps` 和跟踪配置在加载时处理全局继承，业务回调看到的是运行配置。
例如 JSON 中省略通道 `threads` 时，加载器会使用全局 `channel_threads`。
修改配置应通过 Web 或配置文件完成，不要修改 `ctx->config` 指向的数据。

`inference_valid == true` 时，检测结果仍可能为空，这表示本帧没有检测到目标。
多模型通道可能只有部分子模型成功，这个标记不保证所有子模型都成功。

## 视频源与推理区域配置

```text
ctx->config->stream（StreamConfig）
├─ src_type : string                    rtsp / file / usb，必须明确指定
├─ url : string                         RTSP 地址或视频文件路径
├─ device : string                      USB 设备路径
├─ video_enc : string                   h264 / h265
├─ loop : bool                          文件是否循环播放
├─ usb_width : int                      USB 采集宽度
└─ usb_height : int                     USB 采集高度

ctx->config->inference_roi（InferenceRoiConfig）
├─ mode : string                        空表示整帧；支持 roi_only、
│                                       full_plus_roi、full_frame_roi_filter
├─ resize_mode : string                 stretch / expand / letterbox
├─ polygon[] : pair<double,double>      归一化多边形顶点
├─ x / y : double                       加载时计算的归一化包围框起点
└─ width / height : double              加载时计算的归一化包围框尺寸
```

`inference_roi` 决定推理输入或结果限制区域；
`roi_zones` / `rois` 是业务判断使用的区域，例如“入侵区”。
配置中的 `roi_zones[].polygon` 使用归一化坐标，
`ctx->rois` 则已经换算成 640×640 业务坐标。

## 每条模型结果的成员

以下为 `AlgoResult` 的全部数据成员。不同模型使用不同字段，
例如检测模型主要使用框、类别和得分，深度模型使用深度图。

```text
r（AlgoResult，来自 *ctx->results 中的一个元素）
├─ box : cv::Rect                       水平框；旋转框结果在这里放外接水平框
│  └─ x / y / width / height : int      左上角坐标、宽度、高度
├─ has_obb : bool                       是否包含旋转框
├─ obb_corners : array<cv::Point2f,4>    旋转框四个顶点，与 box 同坐标系
├─ label : string                      类别名称
├─ class_id : int                      类别编号
├─ score : float                       当前目标的实际置信度
├─ track_id : int                      跟踪 ID，-1 表示未分配
├─ chn_id : int                        来源通道 ID
├─ frame_id : int64_t                  来源帧号
├─ timestamp_ms : uint64_t             来源帧的分析时间，与该帧 ctx->timestamp_ms 一致
├─ model_id : string                   来源模型 ID
├─ model_type : string                 来源模型类型
├─ model_index : int                   在本次有效模型列表中的顺序
├─ box_color : cv::Scalar              框颜色；(-1,-1,-1) 表示使用默认颜色
├─ keypoints[] : cv::Point2f           姿态关键点坐标
├─ keypoint_scores[] : float           各关键点的置信度
├─ pose_schema : PoseKeypointSchema    None / Coco17 / Hand21
├─ text_result : string                文本结果字段；是否填充取决于模型实现
├─ boxMask : cv::Mat                   分割掩码
├─ depth_map : cv::Mat                 CV_32FC1 深度图，640×640 业务坐标
└─ depth_kind : DepthKind              Metric：米；RelativeInverse：相对逆深度
```

常用结果成员函数：

```text
r
├─ box_center()                        框中心
├─ is_pose() / is_coco17_pose()         判断姿态类型和关键点格式
├─ is_depth()                          是否包含深度图
├─ is_metric_depth()                   是否为米制深度
├─ is_relative_inverse_depth()         是否为相对逆深度
├─ invalid_depth_value()               当前深度类型的无效值
├─ valid_depth(value)                  静态方法：判断米制深度值是否有效
├─ valid_depth_value(value)            按当前深度类型判断数值有效性
├─ depth_valid_at(x, y)                判断某点的深度值是否有效
├─ depth_at(x, y)                      读取深度；无效时返回 0
└─ depth_median(region, step=4)        区域内有效深度的中位数
```

相对逆深度没有“米”的单位，数值越大表示越近，0 和负数也可能有效；
判断有效性应使用 `depth_valid_at()`，不能简单用 `depth_at() > 0`。

## 成员函数入口

下面的分组仅为查阅方便，不是额外的 C++ 成员。例如取业务帧直接写
`ctx->business_frame()`，而不是 `ctx->图像.business_frame()`。

```text
ctx 的公开成员函数
├─ 图像与坐标
│  ├─ business_width() / business_height()    业务坐标宽高，固定 640×640
│  ├─ business_size() / business_center()     业务尺寸和中心点 (320,320)
│  ├─ business_frame()                       惰性读取只读的 640×640 BGR 业务图
│  ├─ source_frame()                         惰性读取原始分辨率的只读 BGR 图
│  ├─ display_canvas()                       获取可写业务显示画布
│  └─ replace_display_frame(frame)           替换当前通道的显示底图
├─ 结果与目标查询
│  ├─ segmentation_result(model_id="")       获取分割结果，未找到返回 nullptr
│  ├─ depth_result(model_id="")              获取深度结果，未找到返回 nullptr
│  └─ query_targets(query)                   组合类别、得分、模型、ROI 条件查询
├─ ROI 查询
│  ├─ roi_count()                            ROI 数量
│  ├─ roi_at(idx)                            按序号取 ROI
│  ├─ roi_polygon_at(idx)                    按序号取多边形
│  └─ roi_by_name(name)                      按名称取 ROI
├─ 当前模块的业务参数
│  ├─ has_param(key)                         是否存在该参数
│  ├─ param_float(key) / param_int(key)       读取浮点数 / 整数
│  ├─ param_bool(key)                        读取布尔值
│  └─ param_string(key) / param_json(key)     读取字符串 / JSON 文本
├─ 向全局逻辑发布同帧业务变量
│  ├─ publish_string(key, value)
│  ├─ publish_number(key, value)
│  ├─ publish_int(key, value)
│  ├─ publish_bool(key, value)
│  └─ publish_json(key, json)
├─ 跨帧状态
│  └─ get_state<T>(args...)                  首次创建，后续回调复用本实例状态
├─ 时间
│  └─ time_str()                            本地时区 YYYY-MM-DD HH:MM:SS
└─ 跨通道读取
   └─ get_channel_frame_snapshot(id, out)    获取同一发布版本的图像和结果等数据
```

`query_targets()` 返回 `TargetQueryResult`：`status` 是查询状态，`count` 是匹配数量，
`best` 是最高置信度目标的借用指针，`valid()` 判断查询是否有效。查询失败不能当作
“区域内无人”。完整约定见 [目标查询说明](../../docs/sdk/target-query.md)。

`get_channel_frame_snapshot()` 返回成功时，输出的 `ChannelFrameSnapshot` 包含
`logic`、`frame`、`results`、`rois`、`draw_cmds`。
其中 `logic.publication_age_ms` 可用于判断数据是否过期，
`logic.config_generation` 表示产生该业务状态的配置代。完整成员见 [snapshot.h](snapshot.h)。

绘图、上报和部分 ROI 组合查询是独立函数，不是 `ChannelContext` 成员：

```cpp
draw_rect(ctx, box);                    // drawing.h
roi_find(ctx, "entrance");              // channel.h
roi_contains(ctx, box, roi_index);      // channel.h
```

事件上报使用 [events.h](events.h) 的 `report_event()`；
GPIO 和推理开关接口见 [README.md](README.md) 的功能速查。

## 框架私有字段

以下字段确实存在于结构体中，但属于 `private`，业务代码不能直接访问。

```text
ChannelContext 私有字段
├─ business_frame_getter : ChannelFrameGetter    业务帧惰性获取回调
├─ source_frame_getter : ChannelFrameGetter   原始帧惰性获取回调
├─ frame_getter_opaque : void *               取帧回调使用的内部上下文
├─ event_sink : EventSink *                     当前会话的事件接收器（真实持久化或离线捕获）
├─ logic_parameters : const LogicParameterSet *  已解析的业务参数
├─ outputs : LogicOutputSet *                本帧业务变量发布容器
├─ draw_cmds : vector<DrawCommand> *          本帧绘图指令容器
├─ canvas : cv::Mat *                        显示底图缓冲
└─ show_canvas : bool *                      是否启用替换显示底图
```

分别通过取帧方法、`param_*()`、`publish_*()`、绘图函数和显示画布方法使用这些能力。
不要把项目业务变量添加到 `ChannelContext`：可配置参数放在模块的 `logic.json`，
跨帧状态放在自己的状态类型中，通过 `get_state<T>()` 获取。

## 阈值与检测结果的对应关系

```text
模型节点配置
  channels[].models[].obj_thresh
                    │
                    ▼
业务配置入口
  ctx->config->models[i].obj_thresh     配置门槛，例如 0.90
                    │
                    ▼
框架进行模型后处理和结果过滤
                    │
                    ▼
业务结果入口
  (*ctx->results)[j].score              实际得分，例如 0.95
  (*ctx->results)[j].model_id           用它匹配 models[i].id
```

检测框交给业务回调时，已经按所属模型的置信度阈值、类别和 NMS 等条件筛选。
业务可以继续加条件，不能从 `results` 中取回已经被过滤掉的目标。
ByteTrack 的低分关联候选只供内部使用；整帧深度数据不参与检测框置信度筛选。

`model_index` 是有效模型列表中的顺序；存在禁用模型时，不应直接拿它索引配置的
`models` 列表。读取对应模型的门槛时，优先使用稳定的 `model_id` 匹配 `id`：

```cpp
// 在通道业务回调内使用；不保存这些引用或指针跨回调。
if (ctx && ctx->config && ctx->results &&
    ctx->infer_enabled && ctx->inference_valid)
{
    for (const auto &result : *ctx->results)
    {
        if (result.is_depth()) continue;
        for (const auto &model : ctx->config->models)
        {
            if (model.id != result.model_id) continue;
            const float threshold = model.obj_thresh; // 配置的筛选门槛
            const float score = result.score;         // 目标实际得分
            // 在这里使用 threshold、score 和 result.box 编写业务逻辑。
            (void)threshold;
            (void)score;
            break;
        }
    }
}
```

## 使用时的生命周期与坐标

- `ctx` 以及从它借用的配置、结果、ROI、图像和状态指针，只在当前回调期间使用。
  普通取帧函数可能返回 `nullptr`；访问前检查。
- `get_state<T>()` 中的对象由框架跨帧保留，不同逻辑实例互相隔离；
  同一实例的帧回调和 Action 要使用相同的状态类型。
- `business_frame()` 的图像是固定 640×640 业务图，
  不代表 RKNN 模型的实际输入尺寸；框、关键点和运行时 ROI 使用业务坐标。
- `source_frame()` 保留视频源原始分辨率。将业务坐标直接画到原始图上之前，需要转换坐标。
- 需要修改或异步保存借用的普通帧图像时，先显式 `clone()`。
  `display_canvas()` 是专门提供的可写显示画布。
- 深度图 `cv::Mat` 支持通过引用计数持有像素跨帧使用；不要缓存所属 `AlgoResult *`。
  修改共享像素前先 `clone()`。
- 时间差计算使用 `timestamp_ms`；显示日期和上报日历时间使用
  `unix_ms` 或 `time_str()`。
- 需要严格区分无效 ROI 时使用 `query_targets()` 并检查返回状态。

新增或调整公共成员时，应同步更新本图、头文件注释及相关 SDK 说明。

