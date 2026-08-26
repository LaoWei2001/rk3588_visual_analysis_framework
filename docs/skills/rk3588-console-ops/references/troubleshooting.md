# 排障手册

先确定故障所在层，不要一开始同时改 logic、配置、服务和远端：

```text
控制台可达/登录
  → App 包完整且进程运行
  → 当前实际配置正确
  → logic/Action 工作
  → event_store 三文件与媒体
  → unified_upload delivery
  → 远端响应
```

## 目录

- [控制台打不开或登录失效](#控制台打不开或登录失效)
- [App 启动失败或异常退出](#app-启动失败或异常退出)
- [实时画面黑屏/报错](#实时画面黑屏报错)
- [Logic 或 Action 不工作](#logic-或-action-不工作)
- [事件没有出现](#事件没有出现)
- [事件存在但不投递](#事件存在但不投递)
- [服务报 CHDIR 或绑定错误](#服务报-chdir-或绑定错误)
- [ROI 或配置看似未生效](#roi-或配置看似未生效)
- [终端或日志 WebSocket 断开](#终端或日志-websocket-断开)

## 控制台打不开或登录失效

```bash
systemctl status rk3588-console.service --no-pager
journalctl -u rk3588-console.service -n 200 --no-pager
curl -s http://127.0.0.1:8080/health
```

登录使用 Linux 系统凭据；后端 session 仅在内存中，控制台重启或 8 小时过期后需重新登录。若前端
源码已改但页面没变，重新执行 `web_console/install.sh` 并确认安装目录的 `frontend/dist` 时间。

## App 启动失败或异常退出

在程序卡片确认二进制、配置文件和所选配置。Web 只允许一个视觉 App；先停止另一个。检查当前
配置：

```bash
app_name='my_app'          # 改成程序卡片上的实际 App 名
config_file='config_6.json' # 改成 run.config 对应的实际文件名
cd "/opt/ai_apps/$app_name"
./vision_analysis --validate-config "./assets/$config_file"
cat run.config
cat run.systemd_unit
```

程序日志来自控制台内存缓冲，不再有 `run.log`。打开 Web“完整日志”；若进程刚异常退出，程序页会
显示最后日志。控制台重启后无法恢复旧 pipe 日志，必要时按 `run.systemd_unit` 查询对应 systemd
service 的 journal。

常见配置边界：有效通道数量、按 ID 排序后的 id/enable 拓扑或显示/RTSP 输出拓扑改变会拒绝热更新；
仅重排配置数组且有效 ID 集合不变不会触发该拒绝。`restart_required` 参数也会整轮拒绝。日志出现
reject 时应重启 App，而不是反复保存。

## 实时画面黑屏/报错

1. 确认唯一运行 App 和 `run.config`。
2. 确认配置 `global.enable_rtsp != 0`、`global.rtsp_codec` 为 `h264`/`avc`。
3. 确认 `rtsp_port/rtsp_path` 与进程日志一致。
4. 确认 `gst-launch-1.0` 存在。
5. 在板端直接验证 RTSP 地址，再检查浏览器 MSE/H264 支持。

Web 后端明确拒绝 H265，不会静默软件转码。刷新另一浏览器会替换同 App 的现有流会话，这是当前
单会话设计。

## Logic 或 Action 不工作

```bash
app_name='my_app' # 改成当前实际 App 名
cd "/opt/ai_apps/$app_name"
./vision_analysis --list-logics
./vision_analysis --list-global-logics
test -S run.control.sock && echo ready
```

再核对实际配置中的 logic ID/全局 `instance_id` 与 App `logics.json`。模块 Action 入队后要等目标通道的
下一业务帧或全局实例下一 tick；断流会让通道 Action 看起来不执行。系统 Action `infer_toggle` 由控制端
立即处理，不走模块 Action 队列。排队后切换 logic 时旧请求会被丢弃。`logic_course_09` 和
`logic_course_10` 当前是空骨架；SOP 对应模块当前缺失。

## 事件没有出现

先记录 `EventReportResult.status/detail`：

- `NO_DELIVERY`：画布没有有效 delivery，契约/连接/revision 为空或 event filter 不匹配；
- `DISABLED`：policy 禁用；
- `INVALID_REQUEST`：事件类型/上下文/视频来源错误；
- `WORKER_UNAVAILABLE`：本地 worker 启动失败；
- `STORAGE_ERROR`：状态无法序列化；
- `CREATED*` 后暂时没有目录：持久化异步，稍后查日志和目录权限。

Web 运行目录：

```bash
app_name='my_app' # 改成当前实际 App 名
find "/opt/ai_apps/.data/$app_name/event_store" -maxdepth 2 -type f -print
```

每条完整事件至少有 `event.json`、`media_state.json`、`delivery_state.json`。只看 event.json 无法判断
媒体或远端状态。

## 事件存在但不投递

```bash
systemctl status unified_upload.service --no-pager
systemctl cat unified_upload.service
journalctl -u unified_upload.service -n 300 --no-pager
```

确认 unit 的 WorkingDirectory、`UPLOAD_DATA_DIR`、`EVENT_STORE_DIR` 都绑定当前运行 App；确认
`.data/<App>/connections.yaml` 中连接 ID 和 adapter 与契约一致；确认 revision 文件存在；读取
`delivery_state.json` 的 status/last_error/last_http_status/next_retry_unix_ms。

`retry` 可能正在 10–300 秒退避；`invalid` 多为契约/连接/mapping；`failed` 多为终止性 4xx 或媒体失败。
页面“重试”只重置非 delivered 状态，不会修复错误配置本身。全部 delivered 后目录会被删除，页面
没有记录是正常成功表现之一，但仍应以服务日志/远端确认。

## 服务报 CHDIR 或绑定错误

```bash
systemctl show unified_upload.service -p WorkingDirectory -p Environment
systemctl show ota_agent.service -p WorkingDirectory -p Environment
```

WorkingDirectory 不存在通常是旧 unit 指向已替换/删除的 App。先启动目标视觉 App，再从“系统服务”
对对应服务执行启动或重启，后端会重写 unit。不要仅 `systemctl restart` 一个仍指向旧路径的 unit。

OTA 还需核对 `CONFIG_FILE` 是否等于当前 `run.config`、`OTA_CONFIG_FILE` 是否存在且有
`platform_ws_host`，指令的 channel/model_id 是否分别匹配 `channels[].id/models[].id`。OTA 写入配置
成功后再检查 C++ 模型热加载日志和回滚结果。当前 OTA Agent 不预先校验 `url` 格式或 `type` 是否为
C++ 支持类型，不能把 Agent 的成功反馈单独作为模型已生效的证据。

## ROI 或配置看似未生效

- 确认编辑器保存的文件就是 `run.config` 指向文件；
- ROI 真源是 `channels[].roi_zones[]`，坐标归一化后由 C++ 转模型坐标；
- USB 抓图与运行采集必须使用一致的固定分辨率，否则视野/坐标可能变化；
- 查看 config monitor 是否整轮拒绝；
- 部署/调试切换会主动改 `global.enable_display`，不要把该变化误判为编辑器丢配置。

## 终端或日志 WebSocket 断开

token 过期会以 WebSocket 1008 关闭。重新登录后刷新页面。单个浏览器页面/前端实例最多打开 4 个
终端；后端当前没有设备级会话总数限制。整页刷新会销毁该前端实例的现有 WebSocket/PTY。HTTP 下
浏览器剪贴板 API 受限，终端前端已有原生粘贴回退；HTTPS/localhost 能使用更完整的 Clipboard API。
