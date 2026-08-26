# Web 控制台二次开发

## 当前技术和目录

后端是 FastAPI/Pydantic，入口 `web_console/backend/main.py`；路由在 `backend/routers/`，共享状态与
系统操作在 `backend/services/`。前端是 React 18 + TypeScript + Vite，使用 React Router、Zustand、
Axios、XYFlow 和 xterm。

```text
web_console/
├── backend/
│   ├── main.py                 应用、鉴权中间件、router 注册、SPA 托管
│   ├── routers/                HTTP/WebSocket 边界
│   ├── services/               进程、日志、持久状态、网络、存储
│   └── tests/
├── frontend/
│   ├── src/api/client.ts       API 类型与调用
│   ├── src/pages/              页面
│   ├── src/components/         编辑器/服务/契约组件
│   ├── src/nodes/              XYFlow 节点
│   ├── src/store/              Zustand 状态
│   └── src/utils/              graph ↔ config 转换
├── install.sh
└── stop.sh
```

## 路由和鉴权

`main.py` 给普通 router 加 `/api` 前缀；`logs.py`、`terminal.py` 自带 `/ws` 路径。所有 `/api/*`
默认鉴权，例外是：

- `POST /api/auth/login`；
- `POST /api/apps/{name}/channels/{id}/actions/{action}`；
- `POST /api/apps/{name}/global-logics/{instance_id}/actions/{action}`。

静态 SPA、`/health` 和 logo 也公开。新增普通 API 时不要另建绕过中间件的隐式认证机制。若改动 Action
公开面，必须同时评估部署网络边界并更新文档/测试。

前端 Axios 在 `client.ts` 自动加 Bearer token，401 清 session 并跳转登录。HTTP middleware 在没有
Bearer header 时也接受 `?token=`，供原生 `<img>/<video>` 资源使用；不要误删这条 fallback。日志/
终端 WebSocket 把 token 放 query；新增 WebSocket 必须在路由内自行校验，因为 HTTP middleware 不
覆盖 WebSocket。

## 添加一个普通页面/接口

1. 在 `backend/routers/` 建立窄职责 router，验证所有文件名、路径、systemctl 参数和用户输入。
2. 在 `backend/main.py` import 并 `include_router`。
3. 在 `frontend/src/api/client.ts` 定义请求/响应类型和调用函数。
4. 在 `frontend/src/pages/` 或 `components/` 实现 UI。
5. 在 `App.tsx` 增加 Route；需要侧栏入口时同时增加 NavLink。
6. 加后端 pytest，运行 `python3 -m pytest`；再运行前端 `npm run build`。
7. 用 `web_console/install.sh` 更新已安装控制台，不要只改源码后期待板端实例自动变化。

## 编辑器开发边界

配置编辑器的双向真源是：

- `utils/configToGraph.ts`：现有 JSON → 节点/边；
- `utils/graphToConfig.ts`：节点/边 → JSON；
- `store/editorStore.ts`：节点、边、dirty、应用集成状态；
- `NodeConfigPanel.tsx`：按节点类型编辑；
- `GlobalSettingsPanel.tsx`：`global` 配置；
- `ConfigPreviewPanel.tsx`：保存前完整结果。

新增配置字段时必须同时核对 C++ parser、两个转换方向、默认值、编辑控件、预览、保存后热重载行为和
round-trip 测试。不能只在一个 Node 组件中加 UI。

Logic 列表/参数/outputs/事件/Action 来自 App `logics.json`。前端不得硬编码新增 logic ID；源码模块
通过生成器进入 catalog 后才会出现。当前唯一例外是 SOP 节点硬编码 `logic_path_sop`，且恰好是已知
缺口，新增功能不要复制这种模式。

上报节点只保存 delivery 对连接和契约 revision 的引用，转换时主动删除 adapter/mapping/request/
success 的冗余副本；完整发送定义来自版本化契约。这一边界不能破坏，否则 Web 预览、C++ outbox 和
Python worker 会各持一份不同协议。

## 实时流、日志和终端

- `stream.py` 只接受 H264/AVC RTSP，GStreamer 零转码封装为 fMP4；前端用 MSE 解析真实 avc codec。
- 同 App 新流会终止旧 GStreamer session，页面重连代码必须避免并发重试风暴。
- App 日志由 `systemd-run --pipe` 读入内存 ring；`/api/apps/{name}/log` 取 tail，
  `/ws/logs/{name}` 推送。
- Terminal 后端为每条 `/ws/terminal` 建 PTY 登录 shell；前端模块级 session registry 在路由切换时
  保留会话，关闭/注销时显式释放。前端 `MAX_TERMINALS=4` 只约束一个前端运行实例，后端路由没有
  实现设备级 PTY 总数上限；若对外开放控制台，容量/安全限制需在后端另行设计。

## 文件和进程安全

沿用现有约束：App 名禁止路径字符和 `.`/`_` 前缀；归档拒绝绝对路径、`..`、symlink；配置只能写
`assets/` 直属 JSON；资产路由只接受受控分类；systemctl unit/action 来自服务端白名单。涉及包替换、
视觉 App 切换和后台服务绑定时使用同一运行锁，避免在检查与写入之间切换 App。

不要在请求协程直接执行长耗时阻塞工作；现有 subprocess/文件重任务按需要放线程或异步子进程。
