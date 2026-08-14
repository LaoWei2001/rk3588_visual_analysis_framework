# RK3588 视觉系统 · 开发/运维知识库索引

> 文档角色：专题知识库索引。项目级导航请先从 [docs 文档总入口](../README.md) 开始；本页负责把已经确定的任务继续路由到具体 Skill 或参考文档。

这个文件夹是给**后续二次开发**用的参考资料，既可以**直接提供给大模型**当上下文，也可以**给人读**。里面有**四个 Skill**：都以 `SKILL.md` 为任务入口；还有一份**源码模块说明** `rk3588-src-modules/`，从其中的 `README.md` 看起。

**先分清四个 Skill 的边界：**

| Skill                      | 管什么                        | 一句话                         |
|:-------------------------- | -------------------------- | --------------------------- |
| **`build-rk3588-vision-app`** | 从自然语言需求到实现和验收的总控流程 | 不确定要改哪一层，或要完整开发一个视觉应用时先用它 |
| **`rk3588-channel-logic`** | 写 C++ 检测/报警逻辑(`logic_xxx`) | "当检测到 X 就做 Y" 这类**画面规则**都归它 |
| **`rk3588-console-ops`**   | 部署、网页控制台(前后端)、后台服务、运维、调试   | Web、服务、部署和排障类任务归它           |
| **`rk3588-global-logic`**  | 写**跨通道 / 周期性**的全局逻辑(`global_xxx`) | "通道 A 有人且通道 B 缺货就报警"这类跨路规则 |

> 另有 **`rk3588-src-modules/`**：C++ 端 config/core/capturer/analyzer/yolo/logic/control/event/recorder/player 等模块，以及外部投递服务边界的深档；想理解或改底层实现时看它。

---

## 我要做 XXX → 看哪里(路由表)

### 从自然语言需求开始

| 我要… | 去看 |
|---|---|
| 描述业务现象，让大模型完成需求拆解、实现、静态验证和交付检查 | `build-rk3588-vision-app/SKILL.md` |
| 先把模型、标签、ROI、状态机、告警媒体和性能要求写成可核对契约 | `build-rk3588-vision-app/references/requirement-contract.md` |
| 开发完成后做功能、媒体、性能和文档验收 | `build-rk3588-vision-app/references/acceptance-checklist.md` |

### 写检测 / 报警逻辑(channel logic)

| 我要…                        | 去看                                                       |
| -------------------------- | -------------------------------------------------------- |
| 第一次从报警判断一路做到 HTTP/Dify 远端成功 | `../报警事件与上报开发指南.md`（人类快速上手 + 大模型提示词 + 完整验收） |
| 把"检测到 X 就报警/上报"做成一个逻辑      | `rk3588-channel-logic/SKILL.md`(总览 + 骨架 + 接线 + 验证 + 坑)   |
| 照着已有的逻辑改，或者参考某个逻辑的编写方式     | `rk3588-channel-logic/references/examples/`(一函数一文件,挑最像的) |
| 查 `ctx` 有哪些字段、辅助函数、绘制、跨帧状态 | `…/references/channelcontext-api.md`                     |
| 搞懂 logic 的命名/注册（函数名·单参数注册·logics.json 的关系）、网页怎么认出逻辑、名字是怎样生成的 | `…/references/logic-naming-and-registration.md` |
| 给逻辑加一个网页能改的参数(半径/秒数/阈值)    | `…/references/adding-config-parameter.md`（模块 Schema + `ctx->param_*()` + Web 热重载） |
| 给实时画面增加自定义按钮、理解按钮到 C++ 的动作链路 | `…/references/custom-button-actions.md`（声明、队列、handler、payload、排错） |
| 第一次跑通上报，或让 logic 提交标准事件并由画布选择 adapter/Profile | `…/references/upload-and-wiring.md`（最小闭环 + 开发契约） |
| 搞懂运行时(9 类主线程、额外后台线程、时序、帧与框同帧、坐标系)  | `…/references/vision_analysis_系统说明文档.md` + `…_架构图.md`        |

### 部署 / 网页控制台 / 后台服务 / 运维 / 调试

| 我要…                                             | 去看                                                          |
| ----------------------------------------------- | ----------------------------------------------------------- |
| 部署到一台新板子、装依赖、编译打包、装程序包                          | `rk3588-console-ops/SKILL.md`(一、二节)                         |
| 知道网页每个功能对应哪个后端路由、落盘到哪                           | `rk3588-console-ops/SKILL.md`(三节)                           |
| **给控制台前端加页面/功能**(React 架构、加接口、WebSocket、改动如何生效) | `…/references/web-console-frontend.md`                      |
| 加/管后台微服务(上报、OTA)、服务配置、systemd 单元                | `…/references/services-upload-and-ota.md`                   |
| 后台服务起不来(CHDIR / 路径失效)、网页↔命令行如何配合                | `…/references/services-upload-and-ota.md` §7 + `SKILL.md` 四 |
| 网页打不开、OTA 没换模型、USB ROI 偏移等已知运维问题                | `rk3588-console-ops/SKILL.md`(四节速查)                         |
| **不知道怎么查 / "改了不生效" / 要系统定位**                    | `…/references/debugging-playbook.md`                        |

### 跨通道 / 周期性全局逻辑

| 我要… | 去看 |
|---|---|
| 聚合多个通道的状态或做跨路联动 | `rk3588-global-logic/SKILL.md`（边界、快照、状态、注册和验证） |
| 在独立线程中做周期巡检 | `rk3588-global-logic/SKILL.md`（`poll_interval_ms` 与 `GlobalContext`） |
| 从全局规则触发图片或视频告警 | 看 `rk3588-global-logic/SKILL.md` 的统一 `report_event(gctx, request)` 与画布接线 |

> 拿不准归哪个领域时先用 `build-rk3588-vision-app`；已经明确时，单通道逐业务帧规则看 `rk3588-channel-logic`，多通道聚合或独立周期轮询看 `rk3588-global-logic`，其余 Web、服务、部署和排障问题看 `rk3588-console-ops`。

---

## 目录地图

```
docs/skills/
├── README.md  ← 你在这
│
├── build-rk3588-vision-app/              自然语言需求→契约→实现→静态验证→验收
│   ├── SKILL.md                          总控流程与领域路由
│   ├── scripts/                          安全脚手架、模块校验和文档审计
│   └── references/                       需求契约与交付验收清单
│
├── rk3588-channel-logic/                 写检测/报警逻辑(通道 logic)
│   ├── SKILL.md                          总览:需求拆解→骨架→接线三件套→验证→坑
│   └── references/
│       ├── channelcontext-api.md         ctx 字段 / 辅助函数 / 绘制 / 跨帧状态
│       ├── logic-naming-and-registration.md  逻辑命名/注册四名关系 + 网页如何识别 + 失配后果
│       ├── adding-config-parameter.md    加可调参数(代码+热重载+网页可配)
│       ├── custom-button-actions.md      Web 自定义按钮(action 声明→Socket→logic handler)
│       ├── upload-and-wiring.md          report_event + report_policy + 画布接线
│       ├── vision_analysis_系统说明文档.md 运行时架构(文字详解)
│       ├── vision_analysis_架构图.md      架构图
│       └── examples/                     当前源码示例和明确标注的业务代码模式
│
├── rk3588-global-logic/                  写跨通道 / 周期性全局逻辑(global_xxx)
│   ├── SKILL.md
│   └── references/two-channel-canvas-demo.md  类型化通道变量→双路聚合→统一告警
│
├── rk3588-console-ops/                   部署 / 控制台 / 服务 / 运维 / 调试
│   ├── SKILL.md                          总览:系统组成、部署、网页功能表、运维速查、文件地图
│   └── references/
│       ├── services-upload-and-ota.md    两个微服务 + systemd 启停配合(网页↔板端)
│       ├── web-console-frontend.md       前端二次开发(架构、加页面/接口、生效流程、坑)
│       └── debugging-playbook.md         调试方法论 + "改了不生效"自查 + 终端实战案例
│
└── rk3588-src-modules/                   C++ 端各源码模块深档(src/ 蒸馏)
    ├── README.md                         模块地图 + 端到端数据流 + 全局约定 + 扩展路由
    └── {runtime,config,core,capturer,analyzer,yolo,logic,control,event,recorder,player,third_party,uploader}.md
```

---

## 怎么用

- **给大模型**：完整需求优先提供 `build-rk3588-vision-app/SKILL.md`，让它按任务路由并完整读取对应领域 Skill；单一明确任务也可直接提供领域 Skill。
- **给人**：新开发者先看总入口；已经明确任务后，从上面的路由表进入。`SKILL.md` 是流程和清单，`references/` 是需要深入时才读的专题。
- **保持它长青**：新增一个正式 `logic_xxx` 后，照 `examples/` 的格式补一篇，并同步总入口的当前模块表；新踩的坑补进 `debugging-playbook.md`，前端/后端新约定补进对应 reference。
