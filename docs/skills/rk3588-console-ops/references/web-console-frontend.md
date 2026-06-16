# Web 控制台「前端」二次开发

控制台前端是一个 **React + Vite + TypeScript 的单页应用(SPA)**,构建出的静态文件由后端 FastAPI 直接托管。要给控制台加页面/功能、或排查前端问题,看本篇。写检测逻辑不归这(那是 `rk3588-channel-logic` skill);后端路由/服务/部署的全貌见本 skill 的 `SKILL.md` 与 `services-upload-and-ota.md`。

## 一、心智模型(先理解数据怎么流)

```
浏览器 (React SPA, 构建产物 dist/)
   │  HTTP /api/*  (axios, 带 Bearer token)
   │  WS   /ws/*   (WebSocket, token 走 ?token= 查询参数)
   ▼
后端 FastAPI (:8080, rk3588-console.service, User=root)
   ├ auth_middleware: 校验 /api/* 的 token(/api/auth/login 例外)
   ├ routers/*.py: 业务路由
   └ 把 frontend/dist 当静态站点托管(SPA fallback 到 index.html)
```

- **前端不直接碰板子**,只调后端的 `/api` 和 `/ws`。要让前端能做新事,通常是「后端加一个路由 + 前端加一个 api 调用 + 一个组件」。
- **构建产物才是线上跑的东西**:你改 `src/` 后必须重新 `npm run build`(`install.sh` 会做),浏览器还要**硬刷新**清缓存。详见 §五。
- **登录态**:`authStore` 里的 token 持久化在 `localStorage('rk3588-auth')`;每个 `/api` 请求由 `api/client.ts` 的拦截器自动带 `Authorization: Bearer`。

## 二、目录结构与各部分职责(`web_console/frontend/src/`)

| 路径 | 职责 |
|------|------|
| `main.tsx` | 入口;**包了 `<StrictMode>`**(开发模式 effect 跑两次,见 §六坑) |
| `App.tsx` | 路由 + 侧边栏外壳(`AppShell`):`<Routes>` 定义页面,`<NavLink>` 定义侧边栏入口,登录守卫 `ProtectedRoute` |
| `api/client.ts` | **所有后端调用的唯一出口**:axios 实例(baseURL `/api`)、Bearer 注入、401 自动登出;每个接口一个导出函数 + TS 类型 |
| `store/` | 全局状态(zustand):`authStore`(登录 token,持久化)、`consoleStore`、`editorStore`、`roiStore` |
| `pages/` | 路由页面:`LoginPage` `AppsPage`(程序管理)`EditorPage`(配置画布)`LogsPage` `RecordsPage` `TerminalPage` |
| `pages/terminalSession.ts` | 终端会话单例/注册表(xterm 实例 + WebSocket 常驻,跨路由保活);非 React 组件 |
| `components/` | 复用组件:`ServicesPanel`(后台服务)`ServiceConfigModal` `NodeConfigPanel`(节点配置面板)`GlobalSettingsPanel`/`GlobalLogicsPanel` `AssetPicker` `NumberField` `ErrorBoundary` 等(ROI 绘制弹窗 `ROIDrawModal` 在 `nodes/ROINode.tsx` 内,不是单独组件) |
| `nodes/` | 编辑器画布的各类节点(React Flow):`StreamNode` `ModelNode` `ROINode` `LogicNode` `ReportNode`(画布 5 种)+ `GlobalNode` / `GlobalLogicNode` |
| `utils/configToGraph.ts` / `graphToConfig.ts` | **配置 ↔ 画布的双向转换**:把 `config.json` 还原成画布节点/连线,以及把画布存回 `config.json` |

## 三、关键约定(照着做就不跑偏)

- **调后端一律走 `api/client.ts`**:在那里加一个导出函数(带 TS 类型),组件里 import 来用。不要在组件里散落裸 `fetch`/`axios`——拦截器的鉴权和 401 处理都在 client 里。
- **WebSocket 不走 axios**:手动 `new WebSocket`,且 **token 必须走查询参数** `?token=...`(WS 和 `<img>` 流没法带 Authorization 头;后端 `auth_middleware` 对带 `?token=` 的请求放行)。范例见 `pages/terminalSession.ts`、`stream` 的 `streamUrl()`。
- **状态用 zustand store**:跨组件共享、需要持久化的状态放 `store/`。`authStore` 是范本(`persist` 中间件 → localStorage)。
- **编辑器是"画布即配置"**:`EditorPage` 用 React Flow 画节点,`configToGraph`/`graphToConfig` 在 `config.json` 和画布之间转换。**逻辑节点的可调参数是数据驱动的**——后端 `/apps/{name}/logics` 透传 App 里的 `logics.json`,`NodeConfigPanel` 的 `LogicForm` 按 `param.type` 自动渲染控件(见 `rk3588-channel-logic` skill 的 `adding-config-parameter.md`)。**给逻辑加参数,前端不用改代码**,加一条 logics.json 声明即可。

## 四、端到端:常见两类改动怎么做

### A. 加一个新页面 + 侧边栏入口
1. `src/pages/XxxPage.tsx` 写页面组件(参考最简的 `LogsPage.tsx`)。
2. `App.tsx` 的 `<Routes>` 里加 `<Route path="/xxx" element={<XxxPage />} />`。
3. `App.tsx` 的侧边栏加 `<NavLink to="/xxx">…</NavLink>`(若需要入口)。
4. 要调后端就去 `api/client.ts` 加函数(见 B)。
5. 样式:同目录加 `XxxPage.css`,在组件里 import。

### B. 让前端能调一个新后端能力
1. **后端**:`backend/routers/` 加路由函数,在 `backend/main.py` 用 `app.include_router(...)` 注册(`/api` 前缀)。鉴权由全局 `auth_middleware` 自动覆盖(除非加进 `_PUBLIC_API`)。
2. **前端 api**:`api/client.ts` 加一个导出函数 + 入参/返回的 TS 类型。
3. **组件**:import 该函数调用,用 `useState`/store 接结果。
4. 部署见 §五(后端改 + 前端改 → 一次 `install.sh` 都带上)。

### C. WebSocket 类功能(终端/实时日志那种)
- 后端:`@router.websocket("/ws/xxx")`,在 `main.py` `include_router`(WS 路由自带 `/ws` 前缀,不加 `/api`)。握手里取 `websocket.query_params["token"]` 校验 `get_session`。
- 前端:`new WebSocket(\`${proto}://${location.host}/ws/xxx?token=${token}\`)`,按需 `binaryType='arraybuffer'`。范例:`pages/terminalSession.ts`。

## 五、改动如何"真正生效"(最容易踩的环节)

**铁律:你在开发机(如 Windows)`src/` 里的改动,对板子毫无影响,直到它被构建并部署到板子的 `/opt/ai_apps/_console`。**

标准部署(后端、前端改动都靠它):
```bash
# 1) 把整个 web_console 同步到板子(开发机执行)
scp -r web_console root@<板子IP>:~
# 2) 在板子上构建 + 部署 + 重启
ssh root@<板子IP> "cd ~/web_console && bash install.sh"
```
`install.sh` 做的事:复制后端 → **板上有 Node 就 `npm run build` 重新构建前端**(没有则用已带的 `dist/`)→ 把 `dist` 复制进 `/opt/ai_apps/_console/frontend/` → 装并 `systemctl restart rk3588-console`。

部署完**务必让浏览器硬刷新**:`Ctrl + Shift + R`。普通刷新会用缓存里的旧 JS(`index-xxxx.js`),让你误以为"改了没用"。

**本地开发模式**(改前端时迭代快,不用每次 install):
```bash
cd web_console/frontend && npm run dev      # vite 起在 5173
```
`vite.config.ts` 已配代理:`/api`→`http://localhost:8080`、`/ws`→`ws://localhost:8080`。也就是说要么本机跑一份后端(`uvicorn`)在 8080,要么把代理目标临时改成板子 IP,即可用本机前端连后端联调。

| 改了什么 | 怎么生效 |
|---------|---------|
| 前端 `src/*` | `install.sh`(重 build)→ **浏览器 Ctrl+Shift+R** |
| 后端 `backend/*` | `install.sh`(重启 rk3588-console)或板上 `systemctl restart rk3588-console` |
| 只换 `logo.png`/`img.png` | 直接覆盖 `frontend/` 下同名文件,后端直接读源文件,无需 build |

## 六、坑(动手前过一遍)

- **以为改了就生效**:没 `install.sh`/没硬刷新 = 板子还在跑旧构建。这是本项目前端调试第一大坑,先排除它再查别的(见 `debugging-playbook.md`)。
- **StrictMode 双跑**:`main.tsx` 包了 `<StrictMode>`,**开发模式**下 effect 会"挂载→卸载→再挂载"。涉及 WebSocket/定时器/订阅的 effect 必须写好 cleanup 且可重入(生产构建不会双跑,但写错了开发期就暴露)。`terminalSession.ts` 的单例就是按可重入设计的。
- **裸用 fetch 绕过 client**:会丢掉 Bearer 注入和 401 自动登出。统一走 `api/client.ts`。
- **WS 忘了带 `?token=`**:握手会被后端关掉(1008)。WS 不能带 Authorization 头,只能查询参数。
- **localStorage 旧值**:前端用 localStorage 存偏好(如 `terminal-fontSize`、auth)。改了相关逻辑后,旧值可能让你看不到新默认——排查时清一下对应 key。
- **SPA 路由刷新 404 的错觉**:后端有 `spa_fallback` 兜底到 `index.html`,直接访问 `/terminal` 也能进;若 404,多半是 `dist` 没部署进去。
