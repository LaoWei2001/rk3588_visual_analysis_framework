# 项目内旧 README 边界

以下 Markdown 位于 `docs/` 之外，仍含 2026-08-22/23 重构前说明。本轮只同步了仓库根 README 的
导航和已经确认的关键示例，没有修改这些靠近源码/服务的旧说明。大模型和二次开发者应使用当前
实现与本 Skill 集，不得从下列旧文字恢复已删除 API 或配置：

| 文件 | 已确认的过期内容 | 当前入口 |
|---|---|---|
| `vision_analysis/src/control/README.md` | 使用已删除的 `ChannelAction`/`ChannelActionResult` 与 `channel_control.cpp`，并把当前公开的 Action POST 写成先登录 | [通道 Action](../../rk3588-channel-logic/references/actions.md) |
| `vision_analysis/src/logic/README.md` | 把缺失的 `logic_path_sop` 及其 `flow` 当作当前模块 | [通道模块索引](../../rk3588-channel-logic/references/current-examples.md) |
| `service/model_update/README.md` | 描述已移除的默认配置/`target_config: active` 推断和旧启动方式 | [服务与部署：OTA](../../rk3588-console-ops/references/services-and-deployment.md#ota-当前契约) |
| `service/upload/README.md` | 声称 custom 同 ID 可覆盖包模板，并使用已不存在的旧契约 ID | [事件与上报开发](../../rk3588-console-ops/references/event-reporting.md#连接与版本化契约) |

这张表不是要求忽略所有模块内 README：例如当前 Dify、全局聚合、event 和 GPIO 说明仍可作为源码
旁注，但运行时行为依然要回到实现、manifest 和生成器核对。发现新冲突时，应在修改对应旧文档或
本表后再交付，不能静默选择一边。
