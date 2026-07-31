# logic_upload_teach — 按钮上报告警示例

- 源码：`vision_analysis/src/logic/modules/logic_upload_teach/logic.cpp`
- 模块清单：同目录 `logic.json`
- 事件类型：`upload_teach_demo`
- 触发方式：Web 动作“生成报警事件”

这是一个最小上报示例：点击 Web 按钮后，action handler 设置 `pending_report`，logic 在下一张有效业务帧调用一次 `report_event()`。请求设置 `merge_mode=EventMergeMode::NEVER`，因此每次点击都会创建独立事件。事件生成的图片、视频和投递适配器完全由当前连接的“上报配置”节点决定；模块本身不绘制额外文字或图形，不声明业务字段，也不直接发送 HTTP 请求。

## 使用方法

1. 在“服务配置”中选择目标 App，新建 adapter Profile；保存后重启已运行的“事件投递服务”。
2. 在目标通道连接持续出帧的视频源和此 logic，选择“按钮上报告警示例”。
3. 从 logic 节点连接一个或多个“上报配置”节点。地址和密钥不在节点里填写；节点选择 Profile
   和接口契约，契约自动带出媒体、固定值和字段映射，节点只保留必要的录像/叠加生成参数。
4. 保存配置并启动 App，确认“告警上报服务”绑定的是这个正在运行的 App。
5. 打开实时画面，在目标通道的“通道控制”区域点击“生成报警事件”。
6. 在 Web“待上报记录”或 `event_store/<event_id>/event.json`、`media_state.json`、`delivery_state.json` 中查看尚未完成的事件，在上传服务日志和远端系统中确认成功结果。

一次点击只提交一个独立事件，但该事件可以按多个上报节点同时生成图片、视频并投递到多个目标。
`EventReportResult.accepted()` 仅表示事件已经进入本地发件箱；失败时日志会输出
`status/detail`。远端投递状态以 delivery 状态、上传服务日志和远端回执为准。所有 delivery
成功后，当前服务会删除事件目录，因此成功记录会从“待上报记录”页面消失。

如果按钮未出现，确认 App 使用的是重新打包后的 `logics.json`，然后关闭并重新打开实时画面。
