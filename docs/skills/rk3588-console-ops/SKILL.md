---
name: rk3588-console-ops
description: >-
  Use, develop, deploy, or troubleshoot this project's RK3588 Web console,
  application packages, local event outbox, HTTP or Dify delivery, OTA service,
  live view, systemd services, logs, records, settings, and terminal. Use when a
  task crosses C++ event creation and Python delivery, and verify paths and API
  behavior against the current Web and service source rather than legacy config files.
---

# RK3588 Web、上报、部署与运维

本 Skill 覆盖当前 Web 控制台、程序包生命周期、事件发件箱、统一上传、OTA 和设备运维。通道/全局
业务规则本身分别由
[`rk3588-channel-logic`](../rk3588-channel-logic/SKILL.md) 和
[`rk3588-global-logic`](../rk3588-global-logic/SKILL.md) 负责。

## 当前部署组成

```text
/opt/ai_apps/
├── _console/                         FastAPI + React 构建产物
├── <App>/                            一个 build.sh 程序包
│   ├── vision_analysis
│   ├── assets/*.json, *.rknn, ...
│   ├── logics.json
│   ├── report_templates/
│   └── services/{upload,model_update}/
└── .data/<App>/                      Web 管理的持久运行数据
    ├── event_store/
    ├── connections.yaml
    ├── report_contracts/
    ├── contract_revisions/
    └── ota_config.json
```

进程归属：

| 进程 | 当前管理方式 |
|---|---|
| `vision_analysis` | Web 通过 `systemd-run` 创建每 App 独立命名的 transient service；全系统只允许运行一个视觉 App |
| Web 控制台 | `rk3588-console.service` |
| 统一上传 | `unified_upload.service`，由 Web 绑定当前运行 App |
| 模型 OTA | `ota_agent.service`，由 Web 绑定当前运行 App 和配置文件 |

## 先判断任务落点

- 操作页面、编辑配置、实时画面：读[Web 用户手册](references/web-console-user-guide.md)。
- 新增或排查上报链路：读[事件与上报开发](references/event-reporting.md)。
- 构建、安装、systemd、OTA：读[服务与部署](references/services-and-deployment.md)。
- 修改 React/FastAPI：读[Web 二次开发](references/web-console-development.md)。
- 现网故障：读[排障手册](references/troubleshooting.md)。

## 端到端工作顺序

1. 在源码侧运行 logic catalog 与配置校验，确认程序包能力真实存在。
2. 用 `vision_analysis/build.sh my_app`（`my_app` 替换为单层包名）生成完整包；`--debug` 仅快速编译二进制，不打包。
3. 用 `install_app.sh` 或 Web“上传程序”安装包，再选择实际配置与部署/调试模式启动。
4. 在画布保存配置；若有投递，在“应用集成”先配置连接和接口契约，再把上报节点连到 logic。
5. 先验证 logic 的 `EventReportResult`，再查 `.data/<App>/event_store`，最后查上传服务日志和远端。
6. 实时预览、Action、记录页和服务页都以当前唯一运行 App 为准。

## 不可混淆的边界

- `report_event()` 只进入本地异步持久化链路，不直接发 HTTP/Dify。
- `accepted()` 不代表文件已经落盘，更不代表远端成功。
- 连接参数当前写入 `connections.yaml`；不存在旧文档中的 `upload_config.yaml`/`config.yaml` 运行契约。
- 接口模板在 App `report_templates/`，用户新建模板在 `.data/<App>/report_contracts/`。
- 全部 delivery 成功后事件目录会删除；“事件投递”页是 outbox，不是成功历史库。
- Web“部署”模式会把所选配置的 `global.enable_display` 写为 0，“调试”模式写为 1。
- 实时画面要求运行配置启用 RTSP 且 codec 为 H264/AVC；它不是逐通道 MJPEG。
- SOP 编辑器仍会生成 `logic_path_sop`，但当前 C++ 未注册该模块，不能作为可运行能力。

## 修改后的验证

Web 后端：

```bash
cd web_console/backend
python3 -m pytest
```

Web 前端：

```bash
cd web_console/frontend
npm run build
```

上传服务：

```bash
cd service/upload
python3 -m pytest
```

涉及 C++/manifest 时还必须运行：

```bash
cd vision_analysis
python3 scripts/generate_logics_catalog.py --check
```

仓库当前没有提交预编译二进制；仅在已经用当前源码得到 `./vision_analysis` 后再运行：

```bash
./vision_analysis --validate-config ./assets/config_6.json
```

`config_6.json` 只是仓库现存示例；验收具体应用时应换成实际运行配置。

测试环境缺少板端 systemd、GStreamer、摄像头或 RKNN 时，应明确区分“静态/单元测试通过”和“板端
实机链路通过”，不能用前者替代后者。
