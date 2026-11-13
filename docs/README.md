# 项目文档

首次使用从[项目首页快速开始](../README.md#快速开始)进入。本文只维护导航；
具体能力、接口和操作步骤在对应主题文档中维护，避免多处复制后产生冲突。

## 按任务选择

| 你要做什么 | 阅读入口 |
| --- | --- |
| 编译、部署、理解配置、接入业务 | [项目指南](guides/project-guide.md) |
| 采集图片或把录像保存到电脑 | [采集指南](guides/dataset-collection.md)、[电脑保存媒体](guides/local-media-saving.md) |
| 停止程序、配置恢复、排查设备问题 | [运维文档](operations/README.md) |
| 理解代码分工和依赖 | [整体架构](architecture/overview.md)、[C++ 源码导航](../engine/README.md) |
| 编写通道/全局 Logic | [公共 SDK](sdk/README.md)、[现有业务示例](skills/rk3588-channel-logic/references/current-examples.md) |
| 查询模型结果、坐标、采集规则 | [技术参考](reference/README.md) |
| 查看性能数据及复现条件 | [性能实测](performance/README.md) |
| 使用开发向导或给代码代理指定任务 | [开发 Skill 索引](skills/README.md) |
| 查看过去的整改和开发记录 | [历史记录](history/README.md) |

最小配置是 [examples/quickstart.json](examples/quickstart.json)，其中视频路径需要替换为实际资源。
不熟悉业务开发时，可从仓库根目录执行 `./develop_feature`；原生 Windows 使用
`develop_feature.cmd` 或 `py .\develop_feature`。支持范围和写入边界见[开发向导](skills/rk3588-feature-wizard/SKILL.md)。

## 目录

```text
docs/
├── README.md          文档总入口
├── guides/            使用、构建与部署指南
├── operations/        停止、恢复、日志和设备诊断
├── reference/         坐标、模型结果与配置语义
├── architecture/      代码职责和数据集架构
├── sdk/               公共业务 API 与验收说明
├── performance/       按条件记录的性能实测
├── history/           历史审查、整改与开发记录
├── skills/            面向开发任务的流程、细分参考与辅助脚本
└── examples/          可复制的配置示例
```

`skills/` 的目录和脚本入口保持稳定；它既供代码代理使用，也可供开发者查阅细分契约。
`performance/` 的历史测量不能直接当成当前版本成绩，`history/` 不作为现行操作手册。

## 事实来源与冲突处理

遇到文档与实现不一致时，依次核对：

1. 当前控制流、公共头文件及相关测试；
2. 模块 `logic.json`、模型/适配器 `catalog.json` 的声明；
3. 实际生成器、配置校验器和同版构建的查询结果；
4. 文档中的说明和示例；历史记录只用于追溯。

公共 API 以 `engine/include/` 为准；配置以 `engine/src/application/config/` 为准；
运行管理以 `services/framework/runtime_manager/python/rk3588_runtime/` 为准。
模块清单和测试结果随版本变化，不在总入口重复维护。

## 维护约定

- 新增主题放入对应目录，并更新该目录的 `README.md`；不要再把专题平铺到 `docs/` 根目录。
- 文件名使用小写短横线；`README.md`、`SKILL.md` 等约定入口名除外。
- 修改接口、配置或生命周期时同步正文；移动文档时同步仓库内链接、代码块路径和锚点。
- 指南说明当前行为；性能报告写明日期、输入、平台和测量口径；历史记录保留当时条件。
- 性能文档保留汇总表、测试条件和统计方法，原始测量文件与运行日志保存在本地测试目录，不纳入仓库；历史“通过”记录不作为当前复验结果。
- 缺少源码或实测依据的结论应注明边界；不通过改文档来掩盖已知代码问题。

本地验证、工作区清理和提交约定见[贡献指南](../CONTRIBUTING.md)。
