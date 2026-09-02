---
name: rk3588-feature-wizard
description: Interactively discover, formalize, implement, and validate a channel or global Logic feature in this RK3588 visual-analysis repository through Codex CLI or Claude Code. Use when a user wants one guided entry point with short environment and requirement confirmations, automatic no-prompt permissions inside an isolated copy, and a mechanically enforced write-back allowlist limited to vision_analysis/src/logic/modules and global_modules.
---

# RK3588 功能开发向导

把一次调用当成一次完整开发会话：先理解需求，再选择已有专项 Skill，最后实施和验收。不得用固定问卷
代替判断，也不得在需求尚有阻塞性歧义时开始修改文件。

## 运行模式

遵守启动提示指定的模式：

- `auto`：默认模式。需求合同完整后先展示合同，然后在同一会话自动开始实现，不再索要一次笼统确认；
- `confirm`：展示合同和预计文件后，等待用户明确确认再实现；
- `plan-only`：只完成澄清、源码核对和实施计划，不修改文件。

用户可以随时纠正答案、改变模式或终止。若实施需要破坏性操作或外部系统变更，即使在 `auto` 模式也
必须停下说明并询问；写入白名单不能通过询问扩大。

## 不可扩大的写入边界

完整读取 [`references/write-boundary.md`](references/write-boundary.md)。通过 `develop_feature` 运行时，
唯一允许回写原仓库的路径是：

- `vision_analysis/src/logic/modules/**`；
- `vision_analysis/src/logic/global_modules/**`。

仓库其他位置只读，包括 `vision_analysis/src/logic/core/**`、配置、测试、文档、Web、服务、脚本和生成物。
不得请求用户扩大白名单，不得要求切换 Full Access、`danger-full-access` 或 `bypassPermissions`，不得创建
指向白名单外的符号链接。其他 Skill 中要求同步框架、Web 或文档的步骤在本向导内一律让位于此边界。

启动器在一次性隔离副本中运行代理，并只在会话正常结束、manifest 校验通过且全部改动都位于白名单时
回写；发现任意越界改动就拒绝整批结果。因此，需要修改其他源码才能成立的需求不是本向导可实现的需求，
只能说明缺失能力并停止。直接手工调用本文件只有指令约束；需要机械保证时必须使用 `develop_feature`。

## 阶段零：确认环境

1. 读取 [`references/agent-adapters.md`](references/agent-adapters.md) 和
   [`references/platform-matrix.md`](references/platform-matrix.md)。
2. 优先使用启动提示中的自动检测报告；手工调用本 Skill 且没有报告时，只读检测 OS、WSL、架构、
   RK3588 设备树、Python、当前编程代理和关键工具。
3. 第一轮先展示检测到的当前开发宿主，并把 RK3588 Linux 明确写成默认部署建议而非已知事实。
4. 第一轮只问“检测结果和默认部署目标是否正确”，提示用户回复“是”即可；不正确时允许回复
   “否 + 简短纠正”。不得要求填写工具、摄像头、NPU、GPIO 或远端服务清单。
5. 用户只回答“否”且没有纠正时，最多追加一个短选项题确认实际开发宿主/部署目标。
6. 硬件或外部服务是否可用由执行者按需求只读检查；无法证明时标成待实机验证。只有它会改变设计时，
   才追加一个可用“是/否”回答的简短问题。用户确认环境前不得询问业务功能或修改文件。

始终区分“当前开发宿主”和“最终部署/验收目标”。在 Windows/macOS/x86_64 Linux 上编写代码，不代表
目标程序运行在这些系统；相反，在 RK3588 板端启动也不代表模型、视频源或 GPIO 已经可用。

## 阶段一：建立事实基线

1. 读取 [`../build-rk3588-vision-app/references/requirement-contract.md`](../build-rk3588-vision-app/references/requirement-contract.md)。
2. 读取 [`references/question-examples.md`](references/question-examples.md)，并把其中的提问格式用于每一轮访谈。
3. 直接读取相关文件建立内容基线；本向导不要求或初始化 Git，启动器会用文件哈希保护已有内容。
4. 根据用户的初始描述，用 `rg` 只读检查相关注册宏、`logic.json`、公共头文件、Web 路由或调用方。
5. 不向用户询问能够从仓库确定的事实，不把历史文档或不存在的模块当作候选能力。
6. 在阶段三开始前不得编辑文件、生成代码、安装依赖或执行会改变项目状态的命令。

## 阶段二：用少量问题理解需求

读取 [`references/question-examples.md`](references/question-examples.md) 并维护业务问题计数。通常只用
2–3 轮，硬上限为 4 轮；初始描述已经回答的内容直接跳过。按顺序使用三个主问题：

1. **业务目标**：仅在初始描述不足时，要求用户用一句话说明最终想看到的结果，并给一个简短完整示例；
2. **行为方案**：先查源码，把通道、输入、触发、复位、重复和异常处理合成一个具体推荐，询问是否按此
   实现；用户可只回答“是”，或用“否 + 不同之处”纠正；
3. **交付方案**：把相关的参数、绘制、输出、事件媒体、远端、Web 和验收合成一个具体推荐，同样让用户
   用“是”或“否 + 不同之处”回答。

只有用户否定却未说明差异、两种实现会产生明显不同结果，或缺少仓库外部契约时，才允许第 4 个业务
问题；它必须是一个推荐是/否题或 2–4 个短选项。达到上限仍无法消除阻塞性歧义时，列出未决项并停止，
不得继续追加问卷或擅自实现。用户回答“不知道”时，先检查源码，再给出有依据的推荐。

以下内容是内部合同字段，不是逐项询问清单；不适用的项目直接标记为不适用：

1. 已确认的开发宿主、目标部署环境、可用硬件和分层验收边界；
2. 业务目标、使用场景和可观察的完成标准；
3. 单通道、跨通道及能否完全由 Logic 模块现有接口实现；
4. 输入视频、模型/标签、ROI、上游 `outputs` 及无推理场景；
5. 当帧条件、持续时间、时间窗口、多目标组合和阈值；
6. 触发、复位、去重、冷却、断流、离线和数据过期行为；
7. 状态生命周期、参数、热重载策略、绘制、输出和 Action；
8. 事件类型、字段、图片/视频/纯数据媒体和合并语义；
9. 远端协议、连接、契约和成功条件；
10. 现有 metadata 驱动的 Web 操作入口、配置往返和失败提示；
11. 性能、硬件、兼容性、非目标和验收条件。

在内部维护“用户原始描述、用户确认的推荐、从源码查明、显式安全假设、待确认”五类信息。命名、文件
位置、已有 API、可检测工具及验证命令由执行者自行确定，不能消耗用户问题额度。

## 完整性门

同时满足以下条件才结束访谈：

- 能用一句无歧义的话描述目标和非目标；
- 已通过短确认接受或纠正开发宿主和默认部署目标；实机能力无法证明时已明确标成待验证；
- 能确定最小正确扩展点及输入/输出契约；
- 功能能完全在两个写入白名单根目录内实现；否则停止而不是通过完整性门；
- 时间、状态、复位和异常行为已确定，或明确不适用；
- 上报和媒体能由模块现有契约完成或明确不需要，且不需要修改 Web 源码或公共引擎；
- 验收条件可由测试、构建、静态检查或明确的板端步骤观察；
- 没有会导致两种明显不同实现的未决问题；达到问题上限仍有此类问题时视为未通过完整性门并停止。

达到完整性门后，向用户展示简洁的需求合同：目标、范围、行为规则、数据/事件契约、预计修改区域、
验证方法、假设和实机待验项。然后按运行模式进入下一阶段。

## 阶段三：路由并实施

先完整读取总控 Skill [`../build-rk3588-vision-app/SKILL.md`](../build-rk3588-vision-app/SKILL.md)，但始终
以本文件的写入白名单覆盖其中更宽的实现范围。再按需求只加载必要专项 Skill：

| 需求 | 专项 Skill |
|---|---|
| 单通道逐帧业务、ROI、状态、输出、按钮 | [`rk3588-channel-logic`](../rk3588-channel-logic/SKILL.md) |
| 跨通道聚合、轮询、全局状态或动作 | [`rk3588-global-logic`](../rk3588-global-logic/SKILL.md) |
| 模块事件、模块内上报模板、现有 Web metadata | 只读参考 [`rk3588-console-ops`](../rk3588-console-ops/SKILL.md) |
| 公共配置、管线、推理、线程或生命周期 | 只读参考 [`rk3588-src-modules`](../rk3588-src-modules/SKILL.md)；不得实施 |

实施时：

1. 用需求合同建立工作计划和验收项；
2. 再次核对目标源码及全部调用方，以源码实际契约修正实施细节；
3. 只修改白名单内明确属于本需求的模块目录，不删除或改名无关模块，不覆盖已有工作区变更；
4. 把 C++、`logic.json` 和需要的 `report_templates/` 全部放在通道/全局模块自己的目录内；
5. 按 [`references/platform-matrix.md`](references/platform-matrix.md) 使用当前系统原生命令，只运行当前
   主机具备依赖的验证，并如实区分已验证、未验证和仅能目标环境验证的项目；
6. 若源码事实推翻需求合同中的假设，暂停实现，只追问这一项关键差异；若解决方式需要越界则直接停止。

## 完成交付

读取 [`../build-rk3588-vision-app/references/acceptance-checklist.md`](../build-rk3588-vision-app/references/acceptance-checklist.md)，
再报告需求解释、修改文件、运行语义、验证结果、剩余实机检查和已知边界。不得把编译成功写成硬件验证，
也不得把事件进入本地队列写成远端投递成功。

终端入口由 [`scripts/start_wizard.py`](scripts/start_wizard.py) 提供，Codex/Claude 的参数差异只维护在
[`scripts/agent_adapters.py`](scripts/agent_adapters.py)；仓库根目录的 `./develop_feature` 只是稳定的
短命令包装器。隔离复制、改动分类和白名单回写由 [`scripts/write_guard.py`](scripts/write_guard.py)
确定性执行，不能用提示词判断代替。
