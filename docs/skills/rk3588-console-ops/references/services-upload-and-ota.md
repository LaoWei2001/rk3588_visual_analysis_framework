# 事件投递与模型 OTA 后台服务

> 第一次接入事件投递时，先完成 [事件上报与画布接线](../../rk3588-channel-logic/references/upload-and-wiring.md) 的最小闭环；本文说明部署、运行与排障。

程序包包含两个独立服务：

- `services/upload/`：消费标准事件发件箱，通过可配置 adapter 投递；
- `services/model_update/`：接收平台 OTA 指令并更新模型。

## 1. 总体关系

```text
channel logic
  └─ report_event(EventRequest)
       └─ /opt/ai_apps/.data/<App>/event_store/<event_id>/
            ├─ event.json
            ├─ media_state.json
            ├─ delivery_state.json
            └─ annotated.jpg / raw.jpg / clip.mp4（按需）
                    │
                    ▼
             EventOutboxForwarder
                    │
                    └─ adapter registry
                         ├─ http_json
                         └─ dify_workflow

OTA 平台 → ota_agent → assets/*.rknn + config.json → C++ 热重载
```

## 2. 事件投递服务 `unified_upload`

### 2.1 代码结构

```text
services/upload/
├─ main.py
├─ event_outbox.py
├─ delivery_tool.py
├─ adapters/
│  ├─ base.py
│  ├─ registry.py
│  ├─ mapping.py
│  ├─ http_json.py
│  ├─ dify_workflow.py
│  └─ catalog.json
└─ config.yaml
```

`EventOutboxForwarder` 只负责扫描、状态机和重试。协议构造及响应判定属于 adapter；
新增协议不应在发件箱循环中增加业务分支。

上图是程序包内的代码和首次默认值。Web 管理模式会把可变数据迁移到
`/opt/ai_apps/.data/<App>/`：`upload_config.yaml`、`contracts/` 和 `event_store/`；同名 App
覆盖、升级甚至只删除程序目录都不会自动删除这份持久数据。

### 2.2 事件发件箱

Web 启动视觉程序和上传服务时会同时设置 `EVENT_STORE_DIR`，标准位置是
`/opt/ai_apps/.data/<App>/event_store/`。源码裸跑未设置环境变量时，C++ 默认当前目录
`./event_store`，上传服务默认其 App 根目录 `event_store/`；无论哪种方式，C++、上传服务和
Web 后端必须指向同一目录。

- `event.json`：标准 `event/source/data.fields` 和策略快照；
- `media_state.json`：每种媒体的 `requested → generating → ready/failed` 状态；
- `delivery_state.json`：每条 delivery 的投递状态、重试次数和错误。

delivery 的 `media` 是所需媒体数组，可取 `annotated_image`、`raw_image`、`video`；
空数组表示纯数据投递。服务只在全部所需媒体 `ready` 后调用 adapter，任一媒体
`failed` 时该 delivery 进入终态 `failed`。

```text
pending → uploading → delivered
                    ├→ retry
                    ├→ invalid
                    └→ failed
```

只有全部 delivery 都成功时事件目录才会被删除；因此 Web 记录页展示的是本地待处理和失败事件，
不是远端成功历史。

网络异常、HTTP 408/425/429 和服务器 5xx 使用 10 秒起步、最长 5 分钟的指数退避持续重试，
没有“12 次后永久失败”的新上限；旧版本留下的“达到 12 次上限”记录会自动恢复。因此断网一天后
可以继续补报，前提是事件目录仍在、服务恢复运行、网络可达，并且这一天内没有触发发件箱容量
回收。C++ 默认总容量为 1 GiB、最低保留磁盘空间为 512 MiB，超限时会从最旧的非生成/非上传
事件开始回收；可用 `EVENT_STORE_MAX_BYTES`、`EVENT_STORE_MIN_FREE_BYTES` 调整。

### 2.3 Profile、接口模板与 adapter

连接配置只使用一个 `profiles` 字典：

```yaml
profiles:
  plant_http:
    adapter: http_json
    url: https://api.example.com/events
    headers:
      Authorization: Bearer replace-me
    timeout: 15

  inspection_dify:
    adapter: dify_workflow
    api_url: https://dify.example.com
    api_key: app-xxx
    timeout: 120
```

delivery 写 `profile_id/contract_id/media`。地址、密钥和 Header 保存在 Profile；
请求方式、媒体、mapping 和成功条件在仓库/程序包默认值中位于
`services/upload/contracts/*.json`；Web 运行值位于 `.data/<App>/contracts/*.json`。Web 选择
Profile 与接口模板，不要求使用者逐条拼装协议。

映射示例：

```json
{
  "profile_id": "plant_http",
  "contract_id": "jnu_alarm_upload",
  "media": ["annotated_image", "raw_image"]
}
```

模板中的 source 以 `event.*`、`source.*`、`fields.*`、`media.*` 开头，也可使用
`constant`；target 支持点路径。具体 adapter 能力由 `adapters/catalog.json` 声明，
接口清单由 `contracts/*.json` 声明。

### 2.4 logic 边界

logic 只描述事件，不了解 HTTP、Dify 或远端字段：

```cpp
EventRequest request;
request.event_type = "person_intrusion";
request.message = "检测到人员进入禁区";
request.fields = {
    event_field("track_id", track_id),
    event_field("score", score),
};

const EventReportResult result = report_event(ctx, request);
if (!result.accepted()) {
    fprintf(stderr, "report rejected: %s (%s)\n",
            event_report_status_name(result.status), result.detail.c_str());
}
```

图片/视频和映射由接口模板决定，画布 report 节点只选择 Profile、模板和事件过滤。
协议变化不应要求修改算法 logic。

### 2.5 预览、测试与排障

Web 的 report 节点支持：

- 使用样例事件预览最终请求，敏感 Header 会被遮蔽；
- 选择本地事件预览真实映射；
- 选择本地事件执行测试发送。

命令行也可直接验证。`delivery_tool.py` 没有 `preview/send` 子命令；默认只预览，增加 `--send`
才真实发送。Web 管理模式必须显式传入持久配置、契约和事件目录：

下面的 `server_22 + jnu_alarm_upload` 来自当前仓库默认配置；如果该 App 的 `.data` 运行配置已被
修改，应换成其中真实存在且 adapter 匹配的 Profile/契约，并让 `media` 与契约完全一致。

```bash
cd /opt/ai_apps/<App>/services/upload
python3 delivery_tool.py \
  --config /opt/ai_apps/.data/<App>/upload_config.yaml \
  --contracts-dir /opt/ai_apps/.data/<App>/contracts \
  --event-dir /opt/ai_apps/.data/<App>/event_store/<event_id> \
  --delivery-json '{"id":"debug","profile_id":"server_22","contract_id":"jnu_alarm_upload","media":["annotated_image","raw_image"]}'

python3 delivery_tool.py \
  --config /opt/ai_apps/.data/<App>/upload_config.yaml \
  --contracts-dir /opt/ai_apps/.data/<App>/contracts \
  --event-dir /opt/ai_apps/.data/<App>/event_store/<event_id> \
  --delivery-json '{"id":"debug","profile_id":"server_22","contract_id":"jnu_alarm_upload","media":["annotated_image","raw_image"]}' \
  --send
```

常规排查顺序：

1. 检查 `EventReportResult.status/detail`；
2. 检查事件目录中的三个 JSON 状态文件；
3. 检查 delivery 的 `profile_id/contract_id`，以及媒体快照是否与契约一致；
4. 用预览确认最终 URL、Header、Body 或 Dify inputs；
5. 查看 `journalctl -u unified_upload -n 100 --no-pager`。

## 3. OTA 服务 `ota_agent`

### 3.1 职责和配置

目录：

```text
/opt/ai_apps/<App>/services/model_update/ota_agent.py
/opt/ai_apps/.data/<App>/ota_config.json
```

`ota_config.json`：

```json
{
  "platform_ws_host": "tunnel.example.com",
  "target_config": "active"
}
```

- `platform_ws_host`：不带协议和路径；服务连接 `wss://<host>/ws/device/<DeviceID>`；
- `target_config`：推荐使用 `active`，每次操作动态读取 App 根目录的 `run.config`；
  也可填写相对 App `assets/` 的明确配置文件名。

环境变量优先级：

- `ASSETS_DIR` 指定目标 App 的 assets 目录；
- `CONFIG_FILE` 覆盖 `target_config`；
- `PLATFORM_WS_HOST` 覆盖平台地址。

网页「服务配置」通过 `ota_config.py` 保存持久文件。OTA 服务只在启动时读取配置，修改后要重启
`ota_agent`。Web 绑定服务时还会把视觉程序实际启动的配置文件名写入 `CONFIG_FILE`，它优先于
`ota_config.json.target_config`，确保 OTA 修改当前运行配置；裸跑时才按上述优先级自行解析。

### 3.2 更新流程

```text
收到 UPDATE_COMMAND
  → 根据 channel + model_id 查 models[].version
  → 版本一致则直接回报成功
  → HTTPS 下载到 /tmp/model_update_chN_<model_id>.rknn
  → MD5 校验
  → 移入 assets/model_chN_<model_id>_<md5前8位>.rknn
  → 按 channels[].id + models[].id 查目标模型
  → 更新该 models[] 元素的 model_path / model_type / version
  → 写回目标配置
  → 回报进度
```

C++ 配置监控检测到 `models[]` 改变后会热换对应通道模型，无需停止业务进程。

### 3.3 多模型定位

OTA 指令必须携带 `model_id`，与通道内稳定的 `models[].id` 完全一致：

```json
{
  "channel": 0,
  "model_id": "detector",
  "type": "yolov5",
  "version": "..."
}
```

服务会先按通道 ID 定位通道，再按模型 ID 定位唯一元素。缺少 `model_id`、ID 不存在或 `models` 不是数组时直接回报失败，不猜测第一个模型。

## 4. systemd 与 Web 面板

### 4.1 两个受管单元

| 服务 key | systemd unit | App 子目录 |
|---|---|---|
| `ota_agent` | `ota_agent.service` | `services/model_update` |
| `unified_upload` | `unified_upload.service` | `services/upload` |

Web 后端 `routers/services.py` 只接受这两个白名单 key，不把用户输入直接拼进 `systemctl`。

推理程序不属于这两个固定后台服务单元。Web 控制台通过 `process_manager` 创建按 App 命名的
systemd transient unit 管理二进制；不要再同时启动旧的固定 `vision_app.service`，否则可能双开。

### 4.2 Web 的自动绑定与启动

Web 面板调用：

```text
POST /api/services/{key}/start
```

后端实际执行：

```text
在全局运行锁内查找当前唯一的视觉 App 和实际启动配置
→ 校验该 App 的 services 子目录
→ 强制覆盖 /etc/systemd/system/<unit>
→ OTA unit 写入 ASSETS_DIR 和 CONFIG_FILE=<视觉程序实际配置>
→ OTA unit 写入 OTA_CONFIG_FILE=.data/<App>/ota_config.json
→ 上传 unit 写入 UPLOAD_DATA_DIR=.data/<App> 和 EVENT_STORE_DIR=.data/<App>/event_store
→ systemctl daemon-reload
→ systemctl disable <unit>
→ systemctl reset-failed <unit>
→ systemctl restart <unit>
```

这里的“绑定”是生成/更新 systemd 单元，不是复制 Python 代码。代码已经随 App 包存在。没有视觉程序运行时，后台服务拒绝启动，不会猜测或绑定程序包列表中的第一个 App。

Web 仍会主动 `disable` 原生 unit 自启，设备启动顺序由 `rk3588-console` 统一编排。页面的「开机自启」勾选状态与用户最后一次启动/停止意图保存在 `/opt/ai_apps/.console_runtime_state.json`。只有 `autostart=true` 且 `desired_running=true` 时，控制台才会先恢复视觉程序，再绑定并恢复对应后台服务。

### 4.3 启动、停止与视觉 App 切换

前端不再提供后台服务绑定 App 下拉框：

- 启动后台服务：自动绑定当前运行的视觉 App；
- 停止后台服务：停止 unit 并记录 `desired_running=false`，但保留自启勾选；
- 启动/切换视觉 App：运行中但绑定不同 App 的后台服务会被自动停止、改绑并重启；
- OTA 除了匹配 App，还必须匹配视觉程序实际加载的 `config*.json`。

后台服务不提供单独的 install 路由。`POST /api/services/{key}/start` 会校验当前运行的视觉
App、写入绑定该 App 的 systemd unit 并启动；`restart` 使用相同绑定流程。

### 4.4 路径失效

后端从 systemd 的 `WorkingDirectory` 判断：

- `path_ok=true`：工作目录存在；
- `path_ok=false`：单元指向已删除或移动的旧 App。

路径失效时，面板显示“自动修复并启动”。后端会按当前运行的视觉 App 覆盖旧单元，无需手动删除 unit 文件。

典型错误：

```text
Failed at step CHDIR
No such file or directory
```

检查：

```bash
systemctl show ota_agent --property=WorkingDirectory
systemctl cat ota_agent
ls -ld <WorkingDirectory>
```

### 4.5 `deploy.sh` 与 Web 自启策略不同

程序包的 `deploy.sh` 可以生成 `vision_app/ota_agent/unified_upload` 单元，并对用户选中的服务执行：

```bash
systemctl enable <service> --now
```

因此：

- `deploy.sh` 选中启动的服务默认开机自启；
- Web 面板默认不开机自启；勾选后由控制台按“勾选 + 最后运行意图”恢复；
- Web 接管后台服务后会 `disable` unit 的原生自启，以保证视觉程序先恢复并正确匹配；
- 两者操作的是同名 unit，后执行者会覆盖前一次单元内容和 enable 状态。

不要把两种入口的自启行为混为一谈。

## 5. 板端操作

### 查看状态与日志

```bash
systemctl status ota_agent unified_upload --no-pager
journalctl -u ota_agent -f
journalctl -u unified_upload -n 100 --no-pager
```

### 启停

```bash
systemctl start ota_agent unified_upload
systemctl stop ota_agent unified_upload
systemctl restart unified_upload
systemctl reset-failed ota_agent
```

### 裸跑调试

裸跑会创建一个独立于 systemd 的进程。开始前先停止对应 unit，调试结束后再恢复，避免两个实例同时处理同一目录或连接同一平台。

```bash
systemctl stop unified_upload
cd /opt/ai_apps/<App>/services/upload
UPLOAD_DATA_DIR=/opt/ai_apps/.data/<App> \
EVENT_STORE_DIR=/opt/ai_apps/.data/<App>/event_store \
python3 -u main.py

systemctl stop ota_agent
cd /opt/ai_apps/<App>/services/model_update
ASSETS_DIR=/opt/ai_apps/<App>/assets \
OTA_CONFIG_FILE=/opt/ai_apps/.data/<App>/ota_config.json \
CONFIG_FILE=<实际运行的config.json> \
python3 -u ota_agent.py
```

## 6. 新增第三个后台服务

1. 在 `service/` 下增加源码目录、入口和配置文件；
2. 修改 `vision_analysis/build.sh` 的 `PYTHON_SERVICES`，确保服务被打入程序包；
3. 在 `web_console/backend/routers/services.py` 的 `MANAGED` 中加入白名单和 unit 模板；
4. 若需要网页配置，增加配置读写路由和前端表单；
5. 明确它与 C++ 的交接协议，优先使用带原子写入和可恢复状态的文件/事件目录；不要照搬已经删除的 Redis uploader；
6. 定义失败重试、幂等、清理和 systemd 重启策略；
7. 文档中说明服务是板级单实例还是允许每 App 多实例。当前 `MANAGED` 的固定 unit 名表示整板每类服务最多一个 Web 受管实例。

## 7. 常见问题

| 现象 | 检查 |
|---|---|
| 上报服务运行但没有事件 | App 是否产生 `event_store/<event_id>/event.json`；单元是否绑定同一 App |
| delivery 一直 retry | `last_error`、网络、远端状态码、媒体文件是否就绪 |
| delivery 变 invalid | Profile 与接口契约是否存在、adapter 是否匹配、媒体快照是否过期、契约 source 是否有效 |
| 配置已保存但服务仍用旧地址 | 重启对应 Python 服务；它不会热重载配置文件 |
| 服务 CHDIR 失败 | systemd `WorkingDirectory` 已失效，在 Web 中重新绑定并启动 |
| 重启设备后 Web 管理的服务没启动 | 在 Web 勾选“开机自启”，并确认上次运行意图仍为启动；控制台按运行状态文件恢复，不要单独手动 enable unit |
| OTA 修改配置但模型没有变化 | `target_config` 是否是运行配置；通道 id 是否匹配；是否使用了优先级更高的 `models[]` |
| 裸跑时重复上传或重复连接平台 | systemd unit 未停止，存在两个服务实例 |
