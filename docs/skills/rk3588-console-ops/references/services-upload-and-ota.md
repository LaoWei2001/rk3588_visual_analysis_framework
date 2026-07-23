# 两个后台微服务：告警上报与模型 OTA

程序包的 `services/` 下包含两个 Python 微服务：

- `services/upload/`：消费统一事件发件箱，按 delivery 投递到业务服务器或 Dify；
- `services/model_update/`：接收平台 OTA 指令，下载模型并修改目标配置文件。

它们与 `vision_analysis` 主进程通过文件交接，不通过 Redis，也不被 channel logic 直接调用。

## 1. 总体关系

```text
channel logic
   │ report_alarm(ctx, type, message, fields)
   ▼
C++ alarm_report
   │ 读取本通道 report_policy / report_parameters
   │
   └─ alarm_store/<event_id>/
        ├─ manifest.json
        ├─ snapshot.jpg / raw.jpg（按策略）
        └─ clip.mp4（按策略，异步完成）
                 │
                 ▼ 扫描事件目录
        unified_upload / EventOutboxForwarder
                 │ 按 manifest.deliveries
                 ├─ server image → HTTP POST
                 └─ dify image/video → 上传文件并运行工作流

OTA 平台
   │ WebSocket UPDATE_COMMAND
   ▼
ota_agent
   ├─ 下载 .rknn 到 /tmp
   ├─ MD5 校验
   ├─ 移入目标 App 的 assets/
   └─ 修改目标 config.json
                 │
                 ▼
        C++ config_monitor → 模型热重载
```

## 2. 告警上报服务 `unified_upload`

### 2.1 代码与入口

源码目录：`service/upload/`；打包后位于：

```text
/opt/ai_apps/<App>/services/upload/
├─ main.py
├─ event_outbox.py
├─ dify_uploader.py
└─ config.yaml
```

`main.py` 创建一个 `EventOutboxForwarder` 工作线程。该线程扫描事件目录，并在处理 delivery 时调用 `DifyUploader`。当前没有 Redis 生产者、消费者、`dify_queue` 或 `BLPOP/RPUSH`。

### 2.2 事件发件箱

默认目录为 App 根目录下的 `alarm_store/`：

```text
/opt/ai_apps/<App>/alarm_store/<event_id>/
```

C++ 主程序通常以 App 根目录为工作目录，默认写 `./alarm_store`；上传服务从 `services/upload/` 向上两级得到同一个目录。两侧都支持用环境变量 `ALARM_STORE_DIR` 覆盖，使用时必须保持一致。

每个事件目录至少包含 `manifest.json`。事件视频可能在报警后窗口结束后才生成，因此服务会等待 `media.video` 指向的文件就绪。

`manifest.json` 中的重要内容：

- `event_id/channel_id/alarm_type/message`；
- `fields`：logic 调用 `report_alarm()` 时提交的运行时字段；
- `channel_parameters`：通道级上报参数；
- `media`：图片、原图和视频文件名；
- `policy_snapshot`：触发时的策略快照；
- `deliveries`：每个投递任务及其状态、重试次数和错误。

### 2.3 delivery 支持范围

| media | target | 当前行为 |
|---|---|---|
| `image` | `server` | 读取 snapshot/raw，组业务 JSON，HTTP POST |
| `image` | `dify` | 上传图片，映射输入字段，运行 Dify 工作流 |
| `video` | `dify` | 等待 MP4，上传视频，运行 Dify 工作流 |

其他组合会被标记为无效投递。

服务器图片请求由 `event_outbox.py::_send_server_image()` 生成固定 JSON：

```json
{
  "source": "JNU",
  "eventType": "4005",
  "detResult": {},
  "snapTime": "...",
  "endTime": "...",
  "base64Data": "<snapshot.jpg base64>",
  "base64DataRaw": "<raw.jpg base64>",
  "invadeFlag": 1,
  "eventId": "..."
}
```

画布只能覆盖 `server_source` 和 `server_event_type`；当前服务器请求不会把 `fields` 或 `delivery.inputs` 写进 `detResult`。如果业务服务器需要算法字段，必须明确修改 Python 协议实现和本文，而不是只在 `logics.json` 增加 `report_fields`。

Dify delivery 的 `inputs` 支持 `event.*`、`channel.*`、`logic.*` 和常量来源；其中 `logic.xxx` 读取 manifest 的 `fields.xxx`。Web 只展示当前 logic 在 `logics.json.report_fields` 声明的字段。

投递状态大致为：

```text
pending → uploading → delivered
                    ├→ retry   （网络异常、非 200、媒体未就绪等）
                    └→ invalid （Profile、字段映射或投递组合无效）
```

远端拒绝或网络失败时，delivery 保留在事件目录中并延迟重试。只有所有 delivery 都是 `delivered` 时，服务才删除整个事件目录。部分成功、重试中或存在 `invalid` 的事件不会被整体删除。

### 2.4 `config.yaml` 与 Profile

连接地址和密钥集中保存在：

```text
/opt/ai_apps/<App>/services/upload/config.yaml
```

结构：

```yaml
dify:
  api_url: "http://dify.example.com"
  api_key: "app-..."
  timeout: 120

server:
  url: "http://server.example.com/api/alarm"
  timeout: 15

profiles:
  line_a_server:
    type: server
    url: "http://server-a.example.com/api/alarm"
    token: ""
    timeout: 15
  line_b_dify:
    type: dify
    api_url: "http://dify-b.example.com"
    api_key: "app-..."
```

画布中的 delivery 只保存 `profile_id` 和投递策略，不把服务地址或密钥写进通道 `config.json`：

- `profile_id` 为空：使用 `server` 或 `dify` 默认连接；
- 指定 `profile_id`：从 `profiles` 查找，且 Profile 的 `type` 必须与 delivery target 一致；
- Profile 不存在、类型不匹配或缺少地址时，该 delivery 进入 `invalid`。

网页「服务配置」弹窗通过 `upload_config.py` 读写这个文件。保存配置不会让已运行的 Python 进程自动重新读取；需要重启 `unified_upload`。

### 2.5 与 channel logic 的关系

logic 只提交业务事件：

```cpp
report_alarm(ctx, "intrusion", "检测到入侵", {
    alarm_field("track_id", result.track_id),
    alarm_field("score", result.score),
});
```

图片/视频、Profile、overlay 和字段映射由画布生成的 `report_policy` 决定。logic 不应：

- 调用已经删除的 `alarm_uploader_enqueue()`；
- 读取 `server_url/dify_api_url/dify_api_key` 旧通道字段；
- 在 C++ 中实现 HTTP 或 Redis 转发；
- 把服务密钥硬编码进业务逻辑。

### 2.6 上报排查

```bash
# 看本地待投递事件
find /opt/ai_apps/<App>/alarm_store -maxdepth 2 -name manifest.json -print

# 查看事件状态
python3 -m json.tool /opt/ai_apps/<App>/alarm_store/<event_id>/manifest.json

# 查看上传服务日志
journalctl -u unified_upload -n 100 --no-pager
journalctl -u unified_upload -f
```

排查顺序：

1. logic 的 `report_alarm()` 是否返回非空事件 ID；
2. `alarm_store/<event_id>/manifest.json` 是否存在；
3. `deliveries[].status/last_error` 是什么；
4. delivery 的 `profile_id` 是否存在且类型正确；
5. systemd 单元绑定的 App 是否正是产生该发件箱的 App。

## 3. OTA 服务 `ota_agent`

### 3.1 职责和配置

目录：

```text
/opt/ai_apps/<App>/services/model_update/
├─ ota_agent.py
└─ ota_config.json
```

`ota_config.json`：

```json
{
  "platform_ws_host": "tunnel.example.com",
  "target_config": "config.json"
}
```

- `platform_ws_host`：不带协议和路径；服务连接 `wss://<host>/ws/device/<DeviceID>`；
- `target_config`：相对 App `assets/` 的配置文件名，必须与当前业务进程实际运行的配置一致。

环境变量优先级：

- `ASSETS_DIR` 指定目标 App 的 assets 目录；
- `CONFIG_FILE` 覆盖 `target_config`；
- `PLATFORM_WS_HOST` 覆盖平台地址。

网页「服务配置」弹窗通过 `ota_config.py` 保存文件。OTA 服务只在启动时读取配置，修改后要重启 `ota_agent`。

### 3.2 更新流程

```text
收到 UPDATE_COMMAND
  → 根据 channel 查本地 version
  → 版本一致则直接回报成功
  → HTTPS 下载到 /tmp/model_update_chN.rknn
  → MD5 校验
  → 移入 assets/model_chN_<md5前8位>.rknn
  → 按 channels[].id 查目标通道
  → 更新 model_path / model_type / version
  → 写回目标配置
  → 回报进度
```

C++ 配置监控检测到 `model_path/model_type` 改变后会热换模型，因此旧单模型配置通常无需停止业务进程。

### 3.3 多模型限制

OTA 当前只更新通道顶层旧字段：

```json
{
  "model_path": "assets/model_ch0_xxxxxxxx.rknn",
  "model_type": "yolov5",
  "version": "..."
}
```

当通道存在非空且有效的 `models[]` 时，推理优先使用 `models[]`。此时只修改顶层 `model_path` 可能会触发一次重载检查，但不会替换实际使用的子模型。

因此当前 OTA 适用于旧单模型通道。若项目使用 `models[]`，需要扩展 OTA 指令和 `ota_agent.py`，明确要更新的 model ID，并修改对应 `models[]` 元素的路径、类型和版本信息。

## 4. systemd 与 Web 面板

### 4.1 两个受管单元

| 服务 key | systemd unit | App 子目录 |
|---|---|---|
| `ota_agent` | `ota_agent.service` | `services/model_update` |
| `unified_upload` | `unified_upload.service` | `services/upload` |

Web 后端 `routers/services.py` 只接受这两个白名单 key，不把用户输入直接拼进 `systemctl`。

推理程序不属于这两个单元。Web 控制台通过 `process_manager` 直接管理 App 二进制；不要再同时启动指向同一 App 的 `vision_app.service`，否则可能双开。

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

旧版 `POST /api/services/{key}/install {app}` 为兼容保留，但请求中的 App 必须等于当前运行的视觉 App，否则返回 409。

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
python3 -u main.py

systemctl stop ota_agent
cd /opt/ai_apps/<App>/services/model_update
ASSETS_DIR=/opt/ai_apps/<App>/assets python3 -u ota_agent.py
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
| 上报服务运行但没有事件 | App 是否产生 `alarm_store/<event_id>/manifest.json`；单元是否绑定同一 App |
| delivery 一直 retry | `last_error`、网络、远端状态码、媒体文件是否就绪 |
| delivery 变 invalid | Profile 是否存在、类型是否匹配、字段映射是否完整 |
| 配置已保存但服务仍用旧地址 | 重启对应 Python 服务；它不会热重载配置文件 |
| 服务 CHDIR 失败 | systemd `WorkingDirectory` 已失效，在 Web 中重新绑定并启动 |
| 重启设备后 Web 安装的服务没启动 | 当前 Web 安装默认执行 `disable`；需要自启时手动 enable |
| OTA 修改配置但模型没有变化 | `target_config` 是否是运行配置；通道 id 是否匹配；是否使用了优先级更高的 `models[]` |
| 裸跑时重复上传或重复连接平台 | systemd unit 未停止，存在两个服务实例 |
