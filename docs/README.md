# RK3588 视觉分析框架文档入口

本目录同时服务两类读者：

- 大模型：按任务加载 `skills/<skill-name>/SKILL.md`，再按其中路由读取必要参考页；
- 二次开发者：从本页选择任务，再阅读同一套 Skill 与 `references/`。

不熟悉框架时，直接从仓库根目录运行交互式开发向导：

```bash
./develop_feature
```

原生 Windows 可运行 `develop_feature.cmd` 或 `py .\develop_feature`。启动器只正式适配 Codex CLI 与
Claude Code：自动探测后选择唯一可用代理，或在两者都可用时请用户选择；也可用 `--agent codex`、
`--agent claude` 显式指定。选定代理后检测 Windows、WSL2、macOS、Linux、CPU 架构、RK3588 设备树和
关键工具；第一题只需用“是”或“否 + 简短纠正”确认检测结果和默认 RK3588 部署目标。业务需求通常
只问 2–3 轮、最多 4 轮；向导先核对源码，再给出合并后的具体方案让用户简短确认，不会要求逐项填写
硬件和内部合同字段。需求合同完整后，只在通道/全局 Logic 模块中实现并按平台能力验证。需要先确认或
只生成计划时，分别使用 `--confirm-before-code`、`--plan-only`。自动实现采用硬写入边界：
代理在一次性副本中无提示执行，原仓库仅回写 `vision_analysis/src/logic/modules/**` 和
`vision_analysis/src/logic/global_modules/**`，发现其他改动便整批拒绝；选择和权限差异见
[`agent-adapters.md`](skills/rk3588-feature-wizard/references/agent-adapters.md)。

本文档已按仓库提交 `6bd2b94dbbdd8787753b90d1527a6882e3a70aa2`（2026-08-23）核对，文档整理日期为 2026-08-25。核对范围包括 C++ 运行时、逻辑模块清单、配置加载与热重载、事件发件箱、Python 投递服务、FastAPI 路由和 React 页面。

## 事实来源与冲突处理

文档不是运行时真值。遇到冲突时按下面顺序判断，并同步修正文档：

1. 当前可执行实现的控制流、公共头文件和对应测试；
2. 模块 `logic.json`、适配器 `catalog.json` 等声明式契约；若其帮助文字与实际控制流冲突，以实现为准并记录缺口；
3. 生成器和配置校验器的实际校验行为；
4. 本目录中的 Skill 与参考页；
5. 示例配置、历史提交和注释。

关键真源：

| 主题 | 真源 |
|---|---|
| 通道 API | `vision_analysis/src/logic/core/channel_logic.h`、`logic_action.h` |
| 全局 API | `vision_analysis/src/logic/core/global_logic.h/.cpp` |
| 逻辑清单 | `vision_analysis/src/logic/modules/*/logic.json`、`global_modules/*/logic.json` |
| 配置与热重载 | `vision_analysis/src/config/`、`src/runtime/app_ctrl.cpp` |
| 事件与媒体 | `vision_analysis/src/event/event_report.h/.cpp`、`src/recorder/` |
| 网络投递 | `service/upload/` |
| Web 行为 | `web_console/backend/`、`web_console/frontend/src/` |
| 打包产物 | `vision_analysis/build.sh` 和两个生成器 |

## 按任务进入

| 任务 | 首选 Skill |
|---|---|
| 从模糊想法开始，只在 Logic 白名单内自动开发 | [`rk3588-feature-wizard`](skills/rk3588-feature-wizard/SKILL.md) |
| 从需求完成一个端到端视觉应用改动 | [`build-rk3588-vision-app`](skills/build-rk3588-vision-app/SKILL.md) |
| 新增/修改逐通道逻辑、参数、ROI、绘制或按钮 | [`rk3588-channel-logic`](skills/rk3588-channel-logic/SKILL.md) |
| 开发多通道聚合、轮询、全局按钮或全局上报 | [`rk3588-global-logic`](skills/rk3588-global-logic/SKILL.md) |
| 使用或扩展 Web、投递、OTA、服务和排障 | [`rk3588-console-ops`](skills/rk3588-console-ops/SKILL.md) |
| 理解/修改引擎模块、配置、线程与生命周期 | [`rk3588-src-modules`](skills/rk3588-src-modules/SKILL.md) |

完整索引见 [`skills/README.md`](skills/README.md)。

## 当前实现快照

### C++ 模块

`vision_analysis/src/` 当前包含：

```text
capturer  common  config  control  display  event  gpio  inference
logic     pipeline  recorder  rtsp  runtime  third_party  tracking  yolo
```

旧目录 `analyzer/`、`core/`、`player/` 已不存在。对应能力分别迁移到 `pipeline/`、`inference/`、`tracking/`、`runtime/`、`display/` 和 `rtsp/`。

### 当前可注册逻辑(若目录与以下列举的不一致则说明开发者删掉了一部分)

通道逻辑：

```text
logic_course_01 ... logic_course_10
logic_course_gpio
logic_default
logic_dify
logic_global_input_demo
logic_relay
```

全局逻辑：

```text
global_channel_aggregate_demo
global_default
```

仓库当前没有提交预编译二进制。仅在完成当前源码的同版构建后核对：

```bash
cd vision_analysis
./vision_analysis --list-logics
./vision_analysis --list-global-logics
```

### 当前上报模型

业务 Logic 只调用 `report_event()`。C++ 把事件异步排入本地 `event_store`，媒体在后台生成；独立 Python 服务读取 `connections.yaml`、版本化接口契约和事件目录，再通过 `http` 或 `dify_workflow` Adapter 投递。`EventReportResult::accepted()` 只表示事件已进入本地持久化队列，不表示已经落盘或远端成功。

### 当前 Web 页面

登录后的侧边栏包含程序管理、实时画面、系统服务、系统设置和终端。配置编辑、日志和待上报记录从具体程序进入。实时画面跟随当前唯一运行的视觉程序；浏览器预览依赖开启内置 RTSP，并要求 `global.rtsp_codec` 为 `h264`/`avc`（Web 编辑器当前固定生成 `h264`）。

## 必须知道的当前边界

1. Web 编辑器仍显示“SOP流程”节点，并会生成 `logic_path_sop`；当前 C++ 模块目录中没有该逻辑。因此它不是可运行能力，不能用于新配置，也不能在文档或提示词中当作现成示例。
2. `logic_periodic_snapshot_demo`、`logic_upload_teach`、`logic_path_sop` 和 `global_two_channel_demo` 均不在当前源码中。现有替代示例分别是 `logic_course_08`、`logic_dify`、`logic_global_input_demo` 与 `global_channel_aggregate_demo`；替代仅表示学习入口，不表示业务语义完全相同。
3. 全局实例的 `channels` 非空时按连接列表取输入；为空时，当前调度实现会取应用全部通道。普通业务应使用 `gctx->inputs()`，它会过滤未发布、离线或过期输入。
4. 全局事件的 `request.source_channel_id` 选择事件来源身份和图片回退来源；事件视频固定使用全局节点的 `media_source_channel_id`。需要视频时必须在 Web 明确选择有效通道。
5. 待上报记录页是本地发件箱视图，不是成功历史。全部 delivery 成功后，投递服务会删除事件目录。
6. `logic_global_input_demo/logic.json` 中 `risk_ratio.help` 仍写着会动态选择“视频通道”，这段帮助文字已落后于实现。当前聚合模块只用它设置 `request.source_channel_id`；视频来源不会随它变化。
7. `config.h` 对 `swap_rb` 的行内注释仍写“不影响上报”，但当前录像实现会在 `video_overlay` 为 `custom`/`all` 时把它应用到事件视频；事件图片仍不受影响。以 recorder 实际分支为准。
8. OTA Agent 当前不单独校验下载 `url` 的格式，也只检查 `type` 非空而不检查它是否属于 C++ 支持的模型类型。下载/写配置成功后仍必须以 MD5、配置监控和模型热加载日志确认最终结果。

项目其他位置仍保留少量重构前 README，本轮因“以 `docs/` 为交付入口、且不改辅助代码”的边界未
整篇重写；不要把它们覆盖本目录契约。逐项冲突见[项目内旧 README 边界](skills/build-rk3588-vision-app/references/known-document-drift.md)。

## 文档目录约定

- 每个 Skill 的 `SKILL.md` 只保留执行流程和参考路由；详细 API、Schema、示例和操作手册放在其 `references/`。
- `development_log.md` 只记录可由 Git 和源码核对的架构节点，不作为当前 API 说明。
- `build-rk3588-vision-app/scripts/` 中的三个辅助脚本创建于 2026-08-14，本轮因“不修改代码”的约束未改；其中 `validate_logic.py` 会错误拒绝当前已支持的全局 Action，另外两个也不覆盖完整的当前契约。限制详见 [`legacy-scripts.md`](skills/build-rk3588-vision-app/references/legacy-scripts.md)；权威验证入口仍是各 Skill 列出的源码生成器和项目测试。
- `rk3588-feature-wizard/scripts/start_wizard.py` 是模型无关的交互入口，`scripts/agent_adapters.py` 封装
  Codex/Claude 的无提示权限差异，`scripts/write_guard.py` 机械执行隔离副本和 Logic 白名单回写；框架
  业务契约仍只来自对应源码与参考页。
- 平台差异和验证边界统一维护在 [`platform-matrix.md`](skills/rk3588-feature-wizard/references/platform-matrix.md)，不得根据操作系统名称臆测硬件可用。

## 文档维护规则

修改下列内容时必须同步更新对应 Skill：

- 移动源码目录或公共头文件；
- 增删逻辑模块、参数、输出、动作、事件或上报模板；
- 改变配置字段或热重载边界；
- 改变事件 Schema、投递状态、持久目录或 Adapter；
- 改变 Web 路由、页面入口、鉴权例外或操作语义。

文档示例只能使用当前源码中存在的名称。无法从源码确认的行为应标成“待验证”，不得补写推测结论。
