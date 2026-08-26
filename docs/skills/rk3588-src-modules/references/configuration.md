# 运行配置参考

当前配置根必须包含 `global` 对象和 `channels` 数组。Web 编辑器是推荐生成入口；C++ 的
`config.cpp/config_init.cpp/config_validator.cpp` 是最终解析与验证真源。

## 目录

- [最小无推理示例](#最小无推理示例)
- [`global`](#global)
- [`channels[]`](#channels)
- [`global.global_logics[]`](#globalglobal_logics)
- [明确拒绝的旧格式](#明确拒绝的旧格式)
- [热重载事务](#热重载事务)

## 最小无推理示例

```json
{
  "global": {
    "enable_display": false,
    "enable_rtsp": true,
    "rtsp_codec": "h264",
    "disp_width": 1280,
    "disp_height": 720,
    "tile_cols": 1,
    "tile_rows": 1,
    "max_fps": 10
  },
  "channels": [
    {
      "id": 0,
      "enable": true,
      "infer_enable": false,
      "stream": {
        "src_type": "rtsp",
        "url": "rtsp://camera/live",
        "video_enc": "h264"
      },
      "models": [],
      "roi_zones": [],
      "logic_parameters": {},
      "report_policy": {"enabled": false, "deliveries": []},
      "report_parameters": {}
    }
  ]
}
```

示例 URL 仅表示结构，必须替换为实际可访问源。

## `global`

当前注册字段：

- 显示：`enable_display`、`disp_width`、`disp_height`、`tile_cols`、`tile_rows`、
  `performance_display`、`debug_display`、`enable_pause_key`；
- RTSP 输出：`enable_rtsp`、`rtsp_port`、`rtsp_path`、`rtsp_bitrate`、`rtsp_codec`、
  `rtsp_encoder`；
- 管线：`channel_threads`、`max_fps`、`local_default_fps`、`queue_size`；
- tracker 默认：`tracker_enable`、`tracker_iou_thresh`、`tracker_max_miss`、
  `tracker_min_hits`；
- 全局业务：`global_logics[]`。

`disp_width/height`、tile rows/cols、max_fps、local_default_fps 必须为正数；开启显示时网格容量不能小于
有效通道数。Web 配置生成器固定 `rtsp_codec: "h264"` 以满足浏览器零转码预览。

## `channels[]`

加载器为未写 id 的项暂用数组序号，但可靠配置应显式写唯一 ID。每一项（包括禁用项）都必须显式
填写受支持的 `src_type`；只有 `enable=true` 的项强制要求非空源位置并进入有效通道列表，随后按 ID
排序。ID 必须位于 `[0, MAX_CHANNEL_NUM)`，因此业务不能依赖输入数组顺序。

字段分组：

- 身份/开关：`id`、`enable`、`infer_enable`、`swap_rb`；
- 输入：`stream`；
- 推理：`models[]`；
- 业务：可选 `logic`、`logic_parameters`、`roi_zones[]`；
- 调度：`threads`、`playback_fps`、`max_fps`；
- tracker override：四个 `tracker_*` 字段；
- 事件：`report_policy`、`report_parameters`。

`swap_rb` 不影响推理和事件图片；它影响实时显示，并且当前在 `video_overlay` 为 `custom`/`all` 时也
会应用到事件视频。`config.h` 中“不影响上报”的旧行内注释未覆盖这条 recorder 实际分支。
`logic` 省略/空表示没有业务后处理。`infer_enable=false` 仍解码/显示，并在 max_fps 节拍以空 results
运行已配置 logic。

### `stream`

`src_type` 必填，仅为 `rtsp`、`file`、`usb`，不再根据 URL 自动推断。

- RTSP/file 使用 `url`；USB 使用 `device` 且必须以 `/dev/video` 开头；
- 启用的 RTSP 通道经过完整验证时，`video_enc` 必须为 `h264` 或 `h265`；file/USB 不校验该字段；
- `loop` 只用于 file；
- `usb_width/usb_height` 为 0 时按 FPS 自动档，否则固定采集尺寸。

file path 可相对 App 工作目录或使用绝对路径。当前初始验证器的 `is_valid_url()` 实际接受
`rtsp://`、`rtsps://`、`http://`、`https://` 或绝对路径，虽然失败提示只写 RTSP/RTSPS；这是一处校验
过宽的实现边界。`src_type: "rtsp"` 的可运行配置仍应使用 `rtsp://` 或 `rtsps://`，不能把验证通过
误当成 GStreamer 的 RTSP 源一定可启动。

### `models[]`

这是唯一模型入口。每项：`id`（通道内唯一）、`enable`、`model_type`、`model_path`、`label_path`、
`version`、`obj_thresh`、`nms_thresh`、`detect_classes`、`npu_core`。

所有模型项的 `id` 都必须非空且在通道内唯一。以下运行字段检查只针对 `enable=true` 的模型：支持
类型为 `yolov5`、`yolov5_seg`、`yolov8_pose`、`yolov8_det`；除 pose 外要求 label，配置了 label
时文件必须存在；阈值在 `[0,1]`；NPU core 为 `-1`/`"auto"` 或 0、1、2。

同一帧可运行多个启用模型，结果通过 `model_id/model_type/model_index` 区分。模型 ID 也是 OTA 定位
契约，不能随意变成数组下标。

### ROI、logic 与事件

`roi_zones[]` 每项为 `name` 和归一化 `[x,y]` polygon。加载时会去掉重复闭合末点，运行快照只保留
至少 3 点并转换为模型输入坐标。旧 `roi_polygon` 已拒绝。

`logic_parameters` 必须是对象，按当前注册 logic 的嵌入 Schema 补默认值和校验。事件 policy 与
delivery 见[事件与上报开发](../../rk3588-console-ops/references/event-reporting.md)。旧的独立
`event_video_*` 字段已拒绝；视频参数只来自 `report_policy`。

## `global.global_logics[]`

每项字段：

- 必填且唯一 `instance_id`；
- `enable`、`logic`；
- `channels`（非空时限定画布输入；空时当前调度器使用全部应用通道）；
- `poll_interval_ms`，最低 10；
- `logic_parameters`；
- `report_policy`、`report_parameters`；
- `media_source_channel_id`。

channels 和媒体来源 ID 必须存在。启用视频 delivery 的全局实例必须显式给有效
`media_source_channel_id`。

## 明确拒绝的旧格式

- `global.model_type/model_path/label_path/obj_thresh/nms_thresh/detect_classes`；
- 通道顶层 `model_type/model_path/label_path/obj_thresh/nms_thresh/detect_classes/npu_core/version`；
- `roi_polygon`；
- `event_video_enable/pre_sec/post_sec/fps/overlay`；
- 旧的扁平 SOP `path_*` 字段；若将来对应模块恢复，应走模块 `logic_parameters.flow`。

## 热重载事务

配置 monitor 每 250 ms 检查文件，需同一 mtime 连续两次才读取。以下变化整轮拒绝并要求重启，且
在任何模型/流副作用前结束：

- 有效通道数、排序后的 id/enable 拓扑；
- `enable_display/enable_rtsp`、显示尺寸/网格、pause key、RTSP 端口/路径/码率/codec/encoder；
- 任一模块参数的 `x-hot-reload: restart_required`。

其余变化分阶段应用：

- 模型/推理/threads 逐通道重载；失败通道回退旧模型字段，其他 logic/ROI/report 仍可发布；
- logic 名或 ROI 变化重置该通道 logic state，`reset_state` 参数也重置，`preserve_state` 保留；
- tracker 参数更新，模型成功切换或 logic 配置改变时按规则重置轨迹；
- 全局实例按 `instance_id` 精确 reconcile；未变化实例不动；
- stream 逐通道停止/重建，新源失败时尽力恢复旧源，失败的新 stream 不写入最终运行快照。

成功读取配置文件不等于所有子系统都接受新值；必须以 config monitor 各阶段日志和最终运行快照为准。
