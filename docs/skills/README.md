# RK3588 视觉系统 · 开发/运维知识库索引

这个文件夹是给**后续二次开发**用的参考资料,既可以**直接喂给大模型**当上下文,也可以**给人读**。里面是两个 skill,各自带一篇总览(`SKILL.md`)+ 若干深档(`references/`)。

**先分清两个 skill 的边界:**

| Skill                      | 管什么                        | 一句话                         |
|:-------------------------- | -------------------------- | --------------------------- |
| **`rk3588-channel-logic`** | 写 C++ 检测/报警逻辑(`logic_xxx`) | "当检测到 X 就做 Y" 这类**画面规则**都归它 |
| **`rk3588-console-ops`**   | 部署、网页控制台(前后端)、后台服务、运维、调试   | 除了写检测逻辑,**其它全归它**           |

---

## 我要做 XXX → 看哪里(路由表)

### 写检测 / 报警逻辑(channel logic)

| 我要…                        | 去看                                                       |
| -------------------------- | -------------------------------------------------------- |
| 把"检测到 X 就报警/上报"做成一个逻辑      | `rk3588-channel-logic/SKILL.md`(总览 + 骨架 + 接线 + 验证 + 坑)   |
| 照着已有的逻辑改，或者参考某个逻辑的编写方式     | `rk3588-channel-logic/references/examples/`(一函数一文件,挑最像的) |
| 查 `ctx` 有哪些字段、辅助函数、绘制、跨帧状态 | `…/references/channelcontext-api.md`                     |
| 给逻辑加一个网页能改的参数(半径/秒数/阈值)    | `…/references/adding-config-parameter.md`(四处对齐 + 热重载)    |
| 逻辑里上报服务器 / Dify(地址跟通道走)    | `…/references/upload-and-wiring.md`                      |
| 搞懂运行时(8 类线程、时序、帧与框同帧、坐标系)  | `…/references/rk3588_yolo_系统说明文档.md` + `…_架构图.md`        |

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

> 拿不准归哪个 skill:**只要不是在写 `logic_xxx` 检测规则,先翻 `rk3588-console-ops`。**

---

## 目录地图

```
.claude/skills/
├── README.md  ← 你在这
│
├── rk3588-channel-logic/                 写检测/报警逻辑
│   ├── SKILL.md                          总览:需求拆解→骨架→接线三件套→验证→坑
│   └── references/
│       ├── channelcontext-api.md         ctx 字段 / 辅助函数 / 绘制 / 跨帧状态
│       ├── adding-config-parameter.md    加可调参数(代码+热重载+网页可配)
│       ├── upload-and-wiring.md          上报(方案2)+ 注册 + logics.json 接线
│       ├── rk3588_yolo_系统说明文档.md     运行时架构(文字详解)
│       ├── rk3588_yolo_架构图.md          架构图
│       └── examples/                     10 个真实逻辑(server/hook/dify/custom/roll…)
│
└── rk3588-console-ops/                   部署 / 控制台 / 服务 / 运维 / 调试
    ├── SKILL.md                          总览:系统组成、部署、网页功能表、运维速查、文件地图
    └── references/
        ├── services-upload-and-ota.md    两个微服务 + systemd 启停配合(网页↔板端)
        ├── web-console-frontend.md       前端二次开发(架构、加页面/接口、生效流程、坑)
        └── debugging-playbook.md         调试方法论 + "改了不生效"自查 + 终端实战案例
```

---

## 怎么用

- **给大模型**:把对应 skill 的 `SKILL.md` 作为上下文喂进去即可——它会按需引用同目录的 `references/`。需求跨两块(如"前端加个功能并排查")就两个 `SKILL.md` 都给。
- **给人**:从上面的**路由表**找到入口;`SKILL.md` 是总览和清单,`references/` 是要深入时才翻的深档。
- **保持它长青**:新增一个 `logic_xxx` 后,照 `examples/` 的格式补一篇;新踩的坑/新定位经验,补进 `debugging-playbook.md`;前端/后端新约定,补进对应 reference。**让文档跟着代码一起长**,这套参考才一直有用。
