# Web 控制台二次开发

## 目录与职责

后端是 FastAPI/Pydantic；前端是 React + TypeScript + Vite，使用 React Router、Zustand、
Axios、XYFlow 和 xterm。目录职责以当前源码为准：

| 路径 | 职责 |
|---|---|
| `web_console/backend/main.py` | 应用生命周期、HTTP 鉴权、路由注册、SPA 托管 |
| `web_console/backend/dependencies.py` | 路由共享的应用路径与输入校验 |
| `web_console/backend/routers/` | HTTP/WebSocket 参数、鉴权和响应边界 |
| `web_console/backend/services/` | Web 业务服务，以及共享运行库的兼容导入 |
| `services/framework/runtime_manager/python/rk3588_runtime/` | 独立于 Web 的进程、持久状态、后台服务和恢复编排 |
| `web_console/frontend/src/api/http.ts` | Axios、登录过期处理、错误转换及在途读取合并 |
| `web_console/frontend/src/api/*.ts` | 按业务拆分的请求；`client.ts` 只保留兼容导出 |
| `web_console/frontend/src/models/` | 脱离 HTTP 的领域类型与数据规范化 |
| `web_console/frontend/src/pages/` | 页面组合与交互 |
| `web_console/frontend/src/components/` | 编辑器、服务、契约等组件 |
| `web_console/frontend/src/features/terminal/` | 终端页面、会话、输入和显示效果 |
| `web_console/frontend/src/nodes/`、`store/`、`utils/` | 画布节点、状态与配置转换 |
| `web_console/scripts/deploy.sh` | 控制台部署实现，由根目录 `install.sh` 调用 |

数据集功能的具体分层与协议见[数据集架构](../../../architecture/dataset-collection.md)。
新增进程管理规则应放共享运行库，避免 Web 和设备守护进程各自实现一份。

## 路由和鉴权

`main.py` 给普通 router 加 `/api` 前缀；日志、终端 router 自带 `/ws` 路径。
普通 `/api/*` 默认要求有效登录，当前例外为：

- `POST /api/auth/login`。
- 通道和全局 logic 的 Action POST：未设置 `LOGIC_ACTION_TOKEN` 时允许未登录调用；设置后须提供
  正确的 `X-Logic-Token`，或使用有效 Web 登录。
- 数据集 receiver 的 poll、image、ack、disconnect 路由使用接收端会话协议；control 路由还要求
  当前会话固定的 `X-Receiver-Control` 密钥。具体公开方法/路径由 `routers/dataset.py` 白名单定义，
  不能将整个 receiver 前缀直接放行。

静态 SPA、`/health` 和 logo 公开。普通请求由 `api/http.ts` 加 Bearer token；HTTP 中间件也接受
`?token=`，供原生 `<img>/<video>` 使用。日志和终端 WebSocket 在路由内自行验证 query token，
不经过 HTTP 鉴权中间件。新增接口应沿用所在协议的认证边界。

## 添加页面或接口

1. 在对应 `backend/routers/` 与 `services/` 增加职责明确的接口和实现；验证文件名、路径和命令参数。
2. 新 router 在 `main.py` 注册；领域类型放相应模块，共用输入边界复用 `dependencies.py`。
3. 前端请求放对应 `api/` 业务模块，公共领域结构放 `models/`，不要继续向兼容入口 `client.ts` 堆实现。
4. 在页面/组件中接入；需要新页面时修改 `App.tsx` 的 Route 和侧栏入口。
5. 按受影响范围运行已有测试，前端运行 `npm run build`；涉及真实设备的行为另做板端验收。
6. 从项目根目录运行 `sudo ./install.sh online` 更新安装实例；已有有效前端构建产物时可用
   `sudo ./install.sh offline`。具体依赖和部署步骤见[服务与部署](services-and-deployment.md)。

## 配置编辑器

配置双向转换分别由 `utils/configToGraph.ts` 和 `utils/graphToConfig.ts` 负责；
`store/editorStore.ts` 管节点、边、dirty 和应用集成状态。`NodeConfigPanel.tsx` 编辑节点，
`GlobalSettingsPanel.tsx` 编辑全局设置，`ConfigPreviewPanel.tsx` 展示最终配置。

新增字段须核对 C++ parser、双向转换、默认值、编辑控件、保存预览与热重载边界，避免只添加 UI。
Logic 列表、参数、outputs、事件和 Action 来自程序包 `logics.json`，前端不得另建硬编码清单。

上报节点只保存连接和契约 revision 引用；转换时删除 adapter/mapping/request/success 的冗余副本。
完整发送定义来自版本化契约，详见[事件与上报](event-reporting.md)。
业务 ROI 与模型推理 ROI 的坐标边界见[业务坐标约定](../../../reference/business-coordinates.md)。

## 实时画面、日志与终端

- `routers/stream.py` 接收 H264/AVC RTSP，使用 FFmpeg stream copy 封装 fMP4；浏览器通过 MSE 播放。
  相同流共用生产者，每个观众独立接收。同一 `client_id` 的新连接替换自己的旧连接。
- 单路查看有本地裁剪和独立预览两种模式。`LiveViewPage.tsx` 查询 `channel-preview-v1` 能力；
  独立预览通过引擎控制 socket 获取、续租和释放通道流，不支持时回退本地裁剪。
  每个通道按源视频尺寸输出，同一通道共用编码流；前后端不传递预览分辨率选项。
  本地裁剪复用 `videoTimelineLayout.ts` 的通道排序和 tile 规则；切换独立通道则需要改变视频连接。
- 原始程序日志由 journald 保存；共享运行库 `log_buffer.py` 维护有界内存 tail 和订阅队列。
  HTTP 日志接口和 `/ws/logs/{name}` 使用该缓冲，不能把它当完整历史档案。
- `services/terminal_sessions.py` 管理 PTY，shell 使用当前登录的 Linux 用户身份。
  后端每个登录 token 最多 8 个会话、全设备最多 32 个；前端一个实例最多 4 个窗格。
- 前端终端 registry 在路由切换时保留会话；意外断线支持 300 秒恢复窗口与带序号的输出重放。
  主动关闭、注销或普通整页离开会显式关闭；关闭消息未送达时由恢复窗口到期清理。

终端会话逻辑在 `terminalSession.ts`、`sessionLifecycle.ts`；显示偏好、字体和效果分别在
`terminalAppearance.ts`、`terminalFonts.ts` 与其他同目录显示组件中。修改外观须保留已有 PTY、
输出背压、鼠标坐标映射、隐藏页面暂停和减少动态效果行为；具体字体目录规则见
`web_console/frontend/src/assets/fonts/README.md`。

现有浏览器测试在 `web_console/frontend/tests/`。模拟 HTTP/WebSocket 的显示测试不能替代真实 PTY、
RTSP 或板端硬件验收；原生全屏测试需要有界面的 Chromium 和有效显示环境。

## 文件和进程边界

App 名禁止路径字符及 `.`/`_` 前缀；归档拒绝绝对路径、父路径和符号链接；配置写入限于受控
`assets/` 直属 JSON，资产分类及 systemctl 单元/动作使用白名单。
程序安装、替换、启停、删除与后台服务绑定使用共享运行锁；包校验和准备完成后才停止旧程序。
详情见[部署](services-and-deployment.md)与[程序停止和恢复](../../../operations/application-recovery.md)。

长耗时文件操作或阻塞命令不得直接阻塞请求事件循环；按现有模式使用线程或异步子进程。
