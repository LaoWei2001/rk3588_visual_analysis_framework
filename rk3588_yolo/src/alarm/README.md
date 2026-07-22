# 上报功能二次开发

业务逻辑只负责两件事：声明可上报字段，并在报警发生时提交这些字段的运行时值。媒体类型、地址、图片/视频叠加方式、视频时间窗和接收端字段名均由 Web 配置。

## 1. 声明字段

在 `src/logic/modules/<module_dir>/logic.json` 的 `report_fields` 中列出全部可上报字段。这里是字段元数据，不保存或修改运行时值。

```json
"report_fields": [
  { "key": "label", "type": "string", "label": "目标类别" },
  { "key": "score", "type": "number", "label": "置信度" },
  { "key": "boxes", "type": "json", "label": "目标框列表" }
]
```

`key` 必须与 C++ 的 `request.fields.set_*()` 名称完全一致。支持 `string`、`number`、`boolean` 和 `json`。

## 2. 触发上报

视频流逻辑中的完整调用如下：

```cpp
const std::string event_id = report_alarm(
    ctx,
    "person_enter",
    "检测到人员进入",
    {
        alarm_field("label", result.label),
        alarm_field("score", result.score),
        alarm_json_field("boxes", R"([{"x":10,"y":20}])"),
    });
```

不同逻辑可以传入完全不同的字段列表。`alarm_field()` 自动处理字符串、数字和布尔值；JSON 对象或数组使用 `alarm_json_field()`。不要在业务逻辑中选择图片、视频或发送目标。一个报警事件可以被 Web 配置的多个上报节点分别发送到 Dify 或业务服务器。

## 3. 自定义叠加信息

业务逻辑使用既有绘制 API，并指定上报目标：

```cpp
draw_text(ctx, "warning", cv::Point(20, 40), cv::Scalar(0, 0, 255),
          0.7, 2, DrawCommand::ALL);    // 实时窗口、上报图片及显示画面录像
draw_rect(ctx, result.box, cv::Scalar(0, 0, 255), 2,
          1.0, DrawCommand::IMAGE);     // 只用于上报图片
```

图片选择“原始”时不绘制这些内容。视频选择“与实时播放窗口画面一致”时，录像线程会把源帧缩放到该通道的播放窗口尺寸，再调用与实时显示相同的渲染函数；它不读取 framebuffer，也不依赖显示或 RTSP 推流是否启用。因此希望同时出现在实时窗口和告警视频中的绘制指令应使用 `DrawCommand::DISPLAY` 或 `DrawCommand::ALL`。

录像源帧按通道进入独立的有界队列，由共享工作池公平调度；同一通道始终按帧顺序处理。MP4 编码最多占用一个工作线程，其余线程继续处理各通道源帧，避免某一路封装视频时阻塞其他通道。

## 4. Web 的三种固定投递

- 图片 → Dify：选择原始当前帧或自定义叠加帧，配置文件变量名和只读参数的 Dify 字段名。
- 视频 → Dify：选择原始源视频，或选择与实时播放窗口完全一致的通道显示画面；配置报警前/后时长、FPS、文件变量名和参数字段名。
- 图片 → 服务器：使用固定 JSON，不发送算法参数；Web 只能修改 `source` 和 `eventType`。

SOP 开启“上报正常工序结果”后，会自动复用第一条启用的 Dify 投递连接生成仅 JSON 的发件箱记录；该记录不截图、不录像，也不参与告警合并。对应 Dify 工作流的文件输入变量必须允许为空。

发送是异步的。`alarm_report()` 返回事件 ID，不等待远端响应。失败事件保存在 `alarm_store/<event_id>/manifest.json`，上传服务会自动重试；调试字段映射时应先检查该文件中的 `fields`、`deliveries` 和 `last_error`。
