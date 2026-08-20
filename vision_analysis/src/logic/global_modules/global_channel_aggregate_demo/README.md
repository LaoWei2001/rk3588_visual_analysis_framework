# 多通道聚合教学示例

通道 Logic 用 `publish_*()` 公开变量；本模块通过 `ChannelLogicSnapshot::read_*()`
安全读取。画布有输入连线时处理连入通道，没有连线时处理应用全部通道。

核心代码只有三步：遍历通道、组合变量、调用 `report_event()`。需要固定通道时可直接使用
`gctx->channel(channel_id)`，不要求先在画布连线。
