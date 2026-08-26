# Web 控制台用户手册

默认安装脚本把控制台部署到 `/opt/ai_apps/_console`，监听 `0.0.0.0:8080`。登录使用板端 Linux
系统账号密码；token 保存在浏览器 localStorage，后端 session 在内存中有效 8 小时，控制台重启后
原 session 也会失效。

## 目录

- [页面地图](#页面地图)
- [程序管理](#程序管理)
- [配置编辑器](#配置编辑器)
- [应用集成与上报节点](#应用集成与上报节点)
- [实时画面](#实时画面)
- [事件投递页](#事件投递页)
- [系统服务、设置和终端](#系统服务设置和终端)

## 页面地图

| 路径 | 页面 | 当前用途 |
|---|---|---|
| `/` | 程序管理 | 安装包、配置选择、部署/调试、启停、自启、编辑、日志、事件 |
| `/live-view` | 实时画面 | 当前唯一运行 App 的拼接视频、实时日志和 logic Actions |
| `/editor/:appName` | 配置编辑器 | 图形化生成 `assets/*.json` |
| `/logs/:appName` | 程序日志 | 当前控制台会话内存日志 |
| `/records/:appName` | 事件投递 | 本地 outbox、媒体、JSON、重试与删除 |
| `/services` | 系统服务 | OTA/统一上传状态、启停、自启意图和 journal |
| `/system-settings` | 系统设置 | 存储、网络、时区与定时重启 |
| `/terminal` | 终端命令行 | 单个前端实例最多打开 4 个独立板端登录 shell |
| `/login` | 登录 | Linux/PAM 凭据登录 |

除登录、健康检查和两个 logic Action POST 路径外，`/api/*` 需要有效 session token。常规 Axios
请求放在 Bearer header；后端也接受 `?token=`，供不能自定义 header 的原生 `<img>/<video>` 资源
使用。日志和终端 WebSocket 同样用 query token 鉴权。

## 程序管理

程序卡片来自 `/opt/ai_apps/<App>/`。完整包通常含二进制、`assets/`、`logics.json`、
`report_templates/` 和 `services/`。

- “上传程序”接受 zip、tar.gz、tgz、tar；安装前会停止所有 Web 托管视觉程序。
- 系统只允许运行一个视觉 App；启动第二个会返回冲突，必须先停止当前 App。
- 启动前选择 `assets/` 下的 JSON 配置，以及“部署”或“调试”。
- 部署模式强制当前配置 `global.enable_display=0`；调试模式强制为 1 并尝试连接板端 `:0` 显示。
- “开机自启”记录用户意图，由 Web 控制台启动时恢复视觉 App 和对应后台服务。
- 删除 App 会停进程并删除 `/opt/ai_apps/<App>`；当前后端不会同时删除
  `/opt/ai_apps/.data/<App>`，持久事件/连接仍需按运维策略另行处理。

Web 上传替换同名包时，`.data/<App>` 位于包外而保留；包内手工修改会随新包被替换。

## 配置编辑器

工具栏支持新建、打开 App 内配置、本地导入、导出、另存为和保存；保存目标必须是 `assets/`
直属的 `.json`。未保存修改在离开页面时会确认。

画布节点与配置关系：

| 节点 | 当前配置结果 |
|---|---|
| 视频流 | 一个 `channels[]` 项；支持 RTSP、文件和 USB 来源 |
| YOLO 推理 | 该通道的 `models[]`；同一通道可连接多个模型 |
| ROI 区域 | `channels[].roi_zones[]`；直接关联视频流 |
| 逻辑函数 | 可选的 `channels[].logic` 与 `logic_parameters` |
| 上报配置 | 通道或全局实例的 `report_policy/report_parameters` 中一条 delivery |
| 全局逻辑 | `global.global_logics[]`，输入通道来自画布连线 |
| SOP 流程 | 当前会生成 `logic_path_sop`，但 C++ 模块缺失，暂不可运行 |

没有 YOLO 节点时仍是正常视频通道：`infer_enable=false`、`models=[]`；直接连接 logic 可做传统 CV
或无推理业务，logic 收到空 results。没有 logic 时仍显示/推理/系统绘制，只是不运行自定义业务。

Logic 和参数选项来自 App 根 `logics.json`，不是前端硬编码。上报连接、接口模板和 OTA 参数从
“应用集成”弹窗管理。底部同时显示全局设置和将要保存的完整配置预览。

保存后，正在运行的 C++ 会监控当前配置。普通参数、ROI、模型、流和全局实例可走各自热重载；
有效通道数量或按 ID 排序后的 id/enable 拓扑，以及输出拓扑类变化会拒绝热更新并要求重启。仅重排
配置文件中通道项、但有效 ID 集合不变时，不会因此被判定为拓扑变化。详细边界见
[`rk3588-src-modules` 配置参考](../../rk3588-src-modules/references/configuration.md)。

## 应用集成与上报节点

在“应用集成”中先完成：

1. 新建 `http` 或 `dify_workflow` 连接；
2. 选择程序包模板，或创建绑定某个 logic/事件类型的接口契约；
3. 必要时设置 OTA 平台地址；
4. 保存。

回到画布，把“上报配置”连到会创建事件的通道/全局 logic，选择连接和契约。一个上报节点产生一
条 delivery；同一 logic 可接多个节点。模板决定媒体和字段映射，节点不应复制模板内部 mapping。
当前表单能设置图片叠加、视频前后时长、FPS 和合并窗口，但没有 `video_overlay` 选择器；新节点默认
`custom`，导入配置中的已有值会被保留。

## 实时画面

页面自动跟随当前唯一 `running` App，读取其 `run.config` 对应配置。正确预览必须满足：

- `global.enable_rtsp` 非 0；
- `global.rtsp_codec` 是 `h264` 或 `avc`；
- App 的 RTSP 服务实际可在配置端口/路径访问；
- 板端存在 `gst-launch-1.0`；
- 浏览器支持 MSE/H264。

数据路径是板内 RTSP H264 → GStreamer depay/parse → fragmented MP4 → 浏览器 MSE，不重新编码。
同一 App 当前只保留一个浏览器流会话，新连接会替换旧连接。页面同时显示当前 App 日志和由
`logics.json` 声明的通道/全局 Action。

## 事件投递页

页面读取 `.data/<App>/event_store/` 中仍存在的事件，可查看事件字段、delivery 状态、原图/标注图/
视频，手动重试未成功 delivery，或删除单条/全部事件。

它不是历史报表：所有 delivery 成功后上传服务会删除事件目录，因此成功项会消失。删除操作也会
永久移除本地目录，远端是否留存由远端系统负责。

## 系统服务、设置和终端

- 系统服务页只管理 `ota_agent.service` 与 `unified_upload.service`。启动/重启前必须先运行一个
  包含对应 `services/` 的视觉 App，Web 会重写 unit 绑定该 App。
- “开机自启”是 Web 的编排意图，底层两个 unit 会被 disable，防止脱离视觉 App 单独绑定旧目录。
- 系统设置包括事件数据存储、IPv4 网络切换、时区和每日重启。网络切换走确认/超时回滚事务，
  不要在结果未确认时同时从另一入口修改网络。
- 终端通过 PTY 启动登录 shell，工作目录默认是 `APPS_ROOT`；前端 `MAX_TERMINALS=4` 只限制单个
  浏览器页面/前端运行实例，后端当前没有设备级 PTY 总数限制。关闭分屏、退出登录或整页刷新会
  关闭相应 WebSocket 并回收 shell。
