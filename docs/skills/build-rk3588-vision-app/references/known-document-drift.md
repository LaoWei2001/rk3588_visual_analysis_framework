# 项目内旧 README 边界

下表记录尚未与当前实现完全同步的模块说明。二次开发应结合当前实现与本 Skill 集核对，
不要从过期文字恢复已删除 API 或配置。

| 文件 | 已确认的过期内容 | 当前入口 |
|---|---|---|
| `projects/README.md` | 若示例模块集合滞后，以实际目录和生成器校验为准 | [通道模块索引](../../rk3588-channel-logic/references/current-examples.md) |
| `services/framework/model_update/README.md` | 描述已移除的默认配置/`target_config: active` 推断和旧启动方式 | [服务与部署：OTA](../../rk3588-console-ops/references/services-and-deployment.md#ota-当前契约) |
| `services/framework/upload/README.md` | 声称 custom 同 ID 可覆盖包模板，并使用已不存在的旧契约 ID | [事件与上报开发](../../rk3588-console-ops/references/event-reporting.md#连接与版本化契约) |

这张表不是要求忽略所有模块内 README：例如当前 Dify、全局聚合、event 和 GPIO 说明仍可作为源码
旁注，但运行时行为依然要回到实现、manifest 和生成器核对。发现新冲突时，应在修改对应旧文档或
本表后再交付，不能静默选择一边。
