# RK3588 框架 Skill 索引

每个子目录都是独立 Skill。大模型应先读匹配任务的 `SKILL.md`，只在需要时加载它直接链接的参考页；开发者也按同一路径阅读，避免维护两套互相漂移的说明。

| Skill | 适用任务 | 主要参考内容 |
|---|---|---|
| [`rk3588-feature-wizard`](rk3588-feature-wizard/SKILL.md) | 用 Codex/Claude 简短确认后，只在 Logic 白名单内自动开发 | 隔离副本、无提示权限、写回守卫、平台矩阵、精简提问 |
| [`build-rk3588-vision-app`](build-rk3588-vision-app/SKILL.md) | 从需求拆解到实现、校验、打包和交付 | 需求合同、验收、提示词模板 |
| [`rk3588-channel-logic`](rk3588-channel-logic/SKILL.md) | 单通道逐帧业务、参数、ROI、绘制、输出和按钮 | `ChannelContext`、manifest、Action、现有示例 |
| [`rk3588-global-logic`](rk3588-global-logic/SKILL.md) | 跨通道聚合、周期轮询、全局 Action 与上报 | `GlobalContext`、当前聚合示例 |
| [`rk3588-console-ops`](rk3588-console-ops/SKILL.md) | Web 使用/开发、事件投递、OTA、systemd 和排错 | 用户手册、上报、服务部署、前端后端 |
| [`rk3588-src-modules`](rk3588-src-modules/SKILL.md) | 引擎架构、配置、数据流、线程和模块边界 | 架构、配置、源码模块地图 |

## 组合使用

- 不知道该选哪个 Skill：从仓库根目录运行 `./develop_feature`；向导会选择可用的 Codex/Claude，但只会实施能完全放在通道/全局 Logic 模块目录内的功能。
- 新增“单通道检测并上报”：先用 `build-rk3588-vision-app` 定边界，再读 `rk3588-channel-logic` 和 `rk3588-console-ops` 的上报参考。
- 新增“多通道组合告警”：使用 `rk3588-global-logic`，并让上游通道通过 `outputs`/`publish_*()` 提供契约。
- 修改引擎公共 API 或配置：使用 `rk3588-src-modules`，再回看所有受影响的业务 Skill。
- 只操作设备和 Web：使用 `rk3588-console-ops`，不要加载 C++ 细节。

## 共同硬规则

1. 先查当前源码和生成清单，不凭旧文档补接口。
2. 不手改打包生成的 App 根目录 `logics.json` 或 `report_templates/`。
3. Logic 不做阻塞网络上传；统一走 `report_event()` 和投递服务。
4. 通道私有状态放 `ctx->state`，全局实例状态放 `gctx->state`；不使用无保护的可变全局或 `static` 业务状态。
5. 当前不存在的模块不得写入配置或作为可运行示例。
6. 若源码和本目录冲突，以源码为准，并在同一改动中修复文档。

## 关于保留的旧辅助脚本

`build-rk3588-vision-app/scripts/` 下三个 Python 文件创建于 2026-08-14，本次按用户要求未修改。
`validate_logic.py` 会错误拒绝当前已支持的全局 Action，另外两个也没有覆盖完整的当前契约；具体见
[旧辅助脚本边界](build-rk3588-vision-app/references/legacy-scripts.md)。它们不得单独作为验收依据，当前
权威校验命令写在各 `SKILL.md` 中。
