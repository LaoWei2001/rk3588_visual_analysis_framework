# RK3588 视觉分析框架 · 文档总入口

> 文档角色：本文件是 `docs/` 的唯一导航入口。新开发者、维护者和大模型都先从这里判断任务类型，再进入课程、专题指南或源码参考。
>
> 基线状态：已按本仓库当前源码核对，日期为 2026-07-30。入口唯一不等于本文覆盖所有技术事实；系统行为始终以现行源码、模块清单、前端序列化和后端路由为准。

## 一、按任务选择入口

| 现在要做什么 | 第一入口 | 用途 |
|---|---|---|
| 从零系统学习二次开发 | [二次开发课程大纲](二次开发课程大纲.md) | 学习顺序、练习、产物和验收标准 |
| 新增或修改单通道检测/报警规则 | [通道逻辑开发](skills/rk3588-channel-logic/SKILL.md) | `logic_xxx`、参数、绘制、动作和统一告警 |
| 第一次开发完整报警与上报功能 | [报警事件与上报开发指南](报警事件与上报开发指南.md) | 报警判断、C++事件、按钮、媒体、HTTP/Dify、Web契约和提示词 |
| 已了解报警，只需查上报接线细节 | [事件上报与画布接线](skills/rk3588-channel-logic/references/upload-and-wiring.md) | C++动态字段、Profile、接口契约、媒体和排错 |
| 新增跨通道、聚合或周期巡检规则 | [全局逻辑开发](skills/rk3588-global-logic/SKILL.md) | `global_xxx`、快照、轮询和当前告警边界 |
| 修改 Web、部署、后台服务或排障 | [控制台 / 部署 / 运维](skills/rk3588-console-ops/SKILL.md) | React、FastAPI、应用包、systemd、上传和 OTA |
| 理解或修改 C++ 底层模块 | [源码模块索引](skills/rk3588-src-modules/README.md) | runtime、config、logic、event、recorder 等模块地图 |
| 不确定该看哪个专题 | [开发/运维知识库索引](skills/README.md) | 更细的“任务 → 文档”路由 |
| 让有仓库权限的大模型执行开发任务 | [大模型功能开发提示词模板](大模型开发提示词模板.md) | 按任务套用约束完整的开发模板 |
| 了解当前设计缺口和改进优先级 | [框架设计评审与改进路线](框架设计评审与改进路线.md) | 安全、事件一致性、操作成本、扩展性和测试 |
| 追溯旧方案和历史决策 | [开发日志](development_log.md) | 仅用于历史追溯，不作为当前接口或实现依据 |

快速判断：单通道逐帧业务规则进入“通道逻辑”；多通道聚合或独立周期轮询进入“全局逻辑”；其余 Web、服务、部署和排障问题进入“控制台 / 部署 / 运维”。

## 二、新开发者启动包

“启动包”不是另一套重复文档，而是一次开发任务开始时需要打开的最小资料集：

1. 先读本页，确认任务边界和资料权威顺序；
2. 按 [二次开发课程大纲](二次开发课程大纲.md) 的基础章节建立运行链路、配置和身份模型；
3. 开发报警或事件投递时完整阅读 [报警事件与上报开发指南](报警事件与上报开发指南.md)；
4. 只选择与任务相符的一份 `SKILL.md`，不要一开始通读全部专题；
5. 从 [源码模块索引](skills/rk3588-src-modules/README.md) 进入相关模块，并以真实头文件、调用点和配置链路确认接口；
6. 选择当前源码中最接近的实现作为参考，完成静态检查、构建和针对性验收。

推荐的两条路径：

```text
系统学习：docs/README.md -> 二次开发课程大纲 -> 对应 SKILL -> 源码模块 -> 练习与验收
需求开发：docs/README.md -> 对应 SKILL -> 最接近的当前示例 -> 真实源码 -> 分层验证
```

## 三、当前架构基线

### 3.1 代码与服务边界

| 范围 | 当前职责 |
|---|---|
| `vision_analysis/` | C++ 采集、推理、通道/全局 logic、绘制、录像和本地标准事件 |
| `web_console/` | Web 编辑器、配置序列化、应用进程管理、实时画面和后台服务控制 |
| `service/upload/` | 消费 `event_store`，按连接 Profile 与接口契约调用 adapter 投递 |
| `service/model_update/` | 模型更新与 OTA 服务 |

当前统一事件链路是：

```text
report_event
  -> event_store/<event_id>/
     event.json + media_state.json + delivery_state.json + 媒体
  -> unified_upload
  -> adapter -> 远端接口
```

C++ logic 不直接做 HTTP，也不在 logic 中保存连接地址或密钥。
三个 JSON 分别承载事件/业务数据、媒体状态和投递状态。

### 3.2 当前通道逻辑模块

通道逻辑位于 `vision_analysis/src/logic/modules/<module_dir>/`，每个模块由 C++ 入口和 `logic.json` 共同定义。`REGISTER_LOGIC(logic_xxx)` 中的 C++ 函数名是唯一 logic ID；构建器自动把它写入 Web 清单。

不要在入口文档手抄一份容易过期的“全部模块固定表”。当前源码清单用下面的命令取得：

```bash
cd vision_analysis
python3 scripts/generate_logics_catalog.py --check
./vision_analysis --list-logics   # 已有当前架构二进制时
./vision_analysis --list-global-logics
```

当前推荐参考：

| 目的 | 当前源码 |
|---|---|
| 空白模块 | [logic_default](skills/rk3588-channel-logic/references/examples/logic_default.md) |
| 基础绘制、参数、结果、ROI、状态和按钮课程 | `logic_course_01` ～ `logic_course_07`；按钮见 [logic_course_07](skills/rk3588-channel-logic/references/examples/logic_course_07.md) |
| 按钮触发上报 | [logic_upload_teach](skills/rk3588-channel-logic/references/examples/logic_upload_teach.md) |
| 周期截图与参数热更新 | [logic_periodic_snapshot_demo](skills/rk3588-channel-logic/references/examples/logic_periodic_snapshot_demo.md) |
| 同步传统 CV 原始帧 | `logic_save_frame_pair` |
| SOP 路径业务 | [logic_path_sop](skills/rk3588-channel-logic/references/examples/logic_path_sop.md) |
| 自定义 ROI 进入报警 | [ROI 告警代码模式](skills/rk3588-channel-logic/references/examples/roi-alarm-pattern.md)（不是内置同名模块） |

`logic_course_08` ～ `logic_course_10` 当前仍是课程任务骨架，不应被大模型当成已完成的生产示例。
全局 logic 位于 `src/logic/global_modules/<module_dir>/`，通过
`REGISTER_GLOBAL_LOGIC(global_xxx)` 自注册；通道和全局模块的 `logic.json` 都由构建器聚合。
`src/logic/catalog.json` 只保存模型类型等非模块共享能力。

通道可以不配置 `logic`：此时只运行视频与可选模型管线，模型检测结果仍由框架绘制，不执行任何 `modules/` 业务后处理。包括 `logic_default` 在内的通道模块都不是系统兜底项。

## 四、资料权威顺序

发生冲突时按以下顺序判断，不按文件篇幅、更新时间或示例数量判断：

1. 当前源码、头文件、模块 `logic.json`、前端序列化和后端路由；
2. `docs/skills/rk3588-src-modules/` 中与源码对应的模块说明；
3. 对应 `SKILL.md` 及其 `references/` 专题文档；
4. 课程大纲、当前示例和大模型提示词模板；
5. `development_log.md` 中的历史记录。

本页是“导航权威”，不是“接口权威”。任何文档与现行源码冲突时，以源码行为为准，同时修正文档；历史日志只追加更正说明，不重写当年的决策背景。

## 五、文档角色与维护规则

| 文档类型 | 负责什么 | 不负责什么 |
|---|---|---|
| 总入口（本页） | 任务分流、当前边界、权威顺序 | 完整 API 和实现细节 |
| 课程大纲 | 学习顺序、练习、阶段产物、验收 | 代替真实接口定义 |
| `SKILL.md` | 某类任务的完整操作流程 | 记录所有历史方案 |
| `references/`、源码模块页 | 单一主题或模块的深入说明 | 充当全项目入口 |
| 示例 | 解释当前正式模块的实现方式 | 保证未来 API 自动兼容 |
| 提示词模板 | 约束大模型按当前架构工作 | 代替人工验收 |
| 开发日志 | 保存演进背景和旧决策 | 描述当前基线 |

维护文档时遵守以下规则：

- 新增、删除或重命名正式 channel logic 时，同步更新本页模块表、`skills/README.md` 的目录地图和对应示例；只有课程内容变化时才改课程任务。
- 修改公共 API、配置字段、Web 序列化、服务边界或告警链路时，先改实现与测试，再更新对应源码模块页和专题文档。
- 不手改构建生成的应用根目录 `logics.json`；通道逻辑 ID 以 `REGISTER_LOGIC()` 的函数名为唯一定义源，其余 Web 元数据以各模块 `logic.json` 为定义源。
- 历史日志中的“当前”只代表条目日期当时的状态；新增历史条目必须带日期，不能让读者把快照误认成现状。
- 文档内部优先使用相对链接；入口页只做导航，细节只在一个专题中完整维护，其他文档链接过去而不复制大段说明。

## 六、常用一致性检查

在具备对应依赖的开发环境中按改动范围执行：

```bash
cd vision_analysis

# 检查 manifest、函数注册、参数 Schema 和访问器
python3 scripts/generate_logics_catalog.py --check

# 编译后核对二进制中的真实注册名
./vision_analysis --list-logics
```

构建、安装、前端检查和告警链路验收命令，请继续进入相应 `SKILL.md`，不要把“清单校验通过”当成完整运行验收。
