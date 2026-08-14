# logic_periodic_snapshot_demo — 周期截图与参数热重载演示

- 源码：`vision_analysis/src/logic/modules/logic_periodic_snapshot_demo/logic.cpp`
- 参数定义：同目录 `logic.json`
- 事件类型：`periodic_snapshot_demo`

这个最小示例同时演示模块专有参数、每通道状态、画面绘制和统一图片上报。逻辑每隔指定时间创建一条独立事件，并在画面右上角显示可热更新的数字。

## 两个模块参数

| 参数 | 作用 | 热重载行为 |
|---|---|---|
| `report_interval_sec` | 截图上报间隔，范围 1～86400 秒，默认 10 秒 | `reset_state`：保存新值后清空计时状态，从新的完整间隔重新计时 |
| `display_number` | 画面右上角数字，默认 100 | `preserve_state`：保存新值后下一业务帧更新显示，不打断当前上报计时 |

参数定义和 C++ 读取都只在本模块目录中：

```cpp
const int64_t display_number = ctx->param_int("display_number");
const int64_t interval_sec = ctx->param_int("report_interval_sec");
```

Web 保存的通道配置形态如下，实际文件由 Web 生成，无需手写：

```json
{
  "logic": "logic_periodic_snapshot_demo",
  "logic_parameters": {
    "report_interval_sec": 15,
    "display_number": 2026
  }
}
```

## Web 画布接线

1. 给通道选择“周期截图与热重载演示”；
2. 填写“参数1：截图上报间隔”和“参数2：右上角显示数字”；
3. 从 logic 节点连接上报节点，选择发送连接和已有接口模板；
4. 没有合适模板时点击“新建接口模板”。编辑器会在“当前算法”分组自动显示：
   `fields.display_number`、`fields.report_interval_sec`、`fields.report_sequence`；
   还可选择 `event.type`、`event.snap_time`、`source.channel_id`、图片和固定值；
5. 给每个 source 填写服务器或 Dify 所需的远端字段名，保存模板后重启事件投递服务；
6. 在上报节点的“带标注图片的叠加内容”中选择：
   - “与实时播放窗口画面一致”：截图包含右上角数字、检测框和其他实时叠加；
   - “当前原始帧”：截图使用当前未叠加的模型输入帧，不含右上角数字和其他绘制信息；
7. 保存画布配置。之后只修改数字并再次保存，C++ 无需重启或重新编译，下一业务帧即显示新值。

现有服务器固定协议始终同时发送 `base64Data` 和 `base64DataRaw`：前者是上述选项决定的
`annotated.jpg`，后者始终是无叠加的 `raw.jpg`。因此选择“与实时播放窗口画面一致”时，
服务器一次请求即可同时取得带信息截图和未叠加图片；选择“当前原始帧”时两个字段内容都不带
叠加。两张图当前都是模型输入尺寸，不是摄像头原分辨率；Dify 图片投递只发送 `annotated.jpg`。

截图模式属于统一 `report_policy`，因此没有在本逻辑中重复增加第三个参数。连接地址、鉴权、图片编码和网络上传也都由统一告警发件箱及上传服务处理，逐帧 logic 不执行 HTTP 或磁盘编码。

## 上报行为

- 第一次选择该逻辑或间隔参数被修改后，会等待一个完整间隔再上报；
- 通道没有新视频帧时不会凭空生成截图；恢复后只生成当前应触发的一张，不追补中断期间的截图；
- 每次周期事件都设置 `merge_mode=EventMergeMode::NEVER`，不会被默认的事件合并窗口折叠；
- 没有连接并启用图片上报节点时，`report_event()` 返回 `NO_DELIVERY`，不会创建事件；日志最多每分钟提示一次；
- 成功创建的事件携带 `display_number`、`report_interval_sec` 和 `report_sequence` 三个业务字段；
  Web 接口模板编辑器会从模块声明中自动显示它们。`accepted()` 仅表示已进入本地发件箱，
  不代表远端投递已经成功。

右上角数字通过 `DrawCommand::DISPLAY` 绘制。统一截图器在叠加模式下会复用 `DISPLAY` 层；
图片未叠加模式则跳过所有绘制。这也是同一份绘图代码能同时服务实时画面与带信息截图的原因。
