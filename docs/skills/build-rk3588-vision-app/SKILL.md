---
name: build-rk3588-vision-app
description: >-
  Turns a natural-language visual-analysis requirement into a scoped,
  implemented and statically validated RK3588 engine application change. Use
  when creating a new vision algorithm module, combining one or more channels,
  adding ROI/tracking/temporal rules, overlays, alarms, images/videos, report
  fields, logic parameters or Web-configurable actions in this repository. Also
  use to assess whether a requested capability belongs in an application logic
  module or the reusable engine core. Not for model conversion or deployment-only
  operations.
---

# 构建 RK3588 视觉应用

把业务描述收敛成可验证的需求契约，再在现有引擎扩展点上实现。当前源码、模块 `logic.json` 和
生成清单是事实来源；文档只提供路径和约束。遇到不一致时先核对源码并修正文档，绝不凭空补接口。

## 1. 先定位仓库并检查现场

从当前目录向上查找同时包含 `vision_analysis/src/logic/`、
`vision_analysis/scripts/generate_logics_catalog.py` 和 `docs/skills/` 的目录，不写死设备路径。

开始前：

1. 读取 `git status --short`，保留用户已有改动，不覆盖无关文件；
2. 运行 `python3 vision_analysis/scripts/generate_logics_catalog.py --check`；
3. 从目标模型的真实 `labels.txt`、现有配置或用户描述确认类别名；无法确认时把它列为待验收项，
   不猜标签；
4. 查找最接近的当前模块及其 `logic.cpp + logic.json`，不要从旧打包目录、历史日志或生成物复制。

## 2. 路由到正确扩展层

| 需求 | 扩展位置 | 必须完整读取 |
|---|---|---|
| 单路逐业务帧检测、ROI、跟踪、时序、绘制、告警、按钮 | `logic_xxx(ChannelContext*)` | [通道逻辑 Skill](../rk3588-channel-logic/SKILL.md) |
| 多路联动、聚合、独立周期巡检 | `global_xxx(GlobalContext*)` | [全局逻辑 Skill](../rk3588-global-logic/SKILL.md) |
| Web 页面、画布转换、上传/OTA 服务、部署与运维 | 对应控制台或服务模块 | [控制台 Skill](../rk3588-console-ops/SKILL.md) |
| 需要新增所有算法都会复用的能力 | 引擎核心 | 先读 [源码模块索引](../rk3588-src-modules/README.md)，证明现有公共接口无法表达需求 |

一个需求可以同时经过多行，但保持分层：业务判定留在 logic；录图、录像和远端投递走统一事件
链路；Web 只编辑声明式配置。不要为了一个算法修改中央分发表、硬编码 Web 下拉列表或新增专用上传线程。

## 3. 写需求契约再动代码

使用 [需求契约](references/requirement-contract.md) 记录已确认项、源码证据、假设和未决项。至少明确：

- 输入通道、模型、准确类别名、坐标系和 ROI；
- 正例、反例、持续时间、离开复位、冷却、按目标还是按区域去重；
- 实时显示内容与告警媒体内容；
- 图片/视频、事件类型、动态字段及来源通道；
- 需要在 Web 调节的参数、默认值、范围和热重载策略；
- 性能预算、断流/空结果/陈旧结果行为及验收素材。

若缺失信息不影响安全的最小实现，可以采用保守默认值并明确记录；若会改变算法语义、模型标签或
外部接口契约，则先向用户确认。

## 4. 选择最小实现

新模块可先预览或创建骨架：

```bash
python3 docs/skills/build-rk3588-vision-app/scripts/scaffold_logic.py \
  --kind channel --name logic_example --label '示例逻辑' --dry-run
```

去掉 `--dry-run` 才写入。脚本只创建不存在的目录，绝不覆盖文件。通常只修改：

```text
vision_analysis/src/logic/modules/logic_xxx/{logic.cpp,logic.json}
# 或
vision_analysis/src/logic/global_modules/global_xxx/{logic.cpp,logic.json}
```

实现时遵守：

- 注册名是唯一外部 ID；源 `logic.json` 不写 `name`；
- 参数、动作、outputs、事件类型和上报字段都在同目录清单声明；
- 跨帧/跨 tick 状态放 `*ctx->state` / `*gctx->state`，不用可变 `static`；
- 通道 logic 使用模型坐标；源图处理必须显式转换，不能混用坐标；
- 通道对全局公开数据使用 `publish_*()`/`LogicOutputSet`，不强转其他模块私有状态；
- 业务模块不直接做 HTTP、Redis、sleep、阻塞磁盘 I/O、图片编码或视频编码；事件只走
  `report_event()`，其同步工作是引擎约定的少量状态 JSON 原子落盘，媒体编码在后台执行；
- 告警统一调用 `report_event()`；`accepted()` 只表示本地事件已接受，不代表远端成功；
- 绘制使用 `draw_*`，明确 `DISPLAY`/`MEDIA` 层，不自行维护另一套显示管线。

只有当公共能力会被多个算法复用、无法由现有上下文表达且兼容旧调用方式时才修改底层。此时说明
公共契约、线程所有权、生命周期、向后兼容和性能影响，并同步对应 `rk3588-src-modules/` 文档。

## 5. 静态验证

用户没有明确授权时，不编译、不安装、不启动/停止服务、不改设备运行配置。先运行：

```bash
python3 vision_analysis/scripts/generate_logics_catalog.py --check
python3 docs/skills/build-rk3588-vision-app/scripts/validate_logic.py logic_example
python3 docs/skills/build-rk3588-vision-app/scripts/audit_docs.py
```

`validate_logic.py` 检查目录、注册宏、清单 Schema、事件/字段声明和明显的实时路径阻塞调用；
`audit_docs.py` 检查本仓库 Markdown 相对链接、关键入口及已知淘汰说法。警告必须人工判断，错误必须修复。

获得构建授权后，才按目标 Skill 中的构建方法验证；构建通过也不能替代现场算法验收。完整交付检查见
[验收清单](references/acceptance-checklist.md)。

## 6. 交付说明

简洁报告：实现的业务行为、修改文件、参数/事件/字段契约、静态检查结果、未执行的构建或现场测试、
仍需用户提供的模型/素材/阈值。不要把“代码完成”“本地事件创建”“远端发送成功”混为同一结论。
