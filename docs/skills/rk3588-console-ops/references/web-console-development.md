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
7. 在项目根目录用 `sudo ./install.sh online` 更新已安装控制台，不要只改源码后期待
   板端实例自动变化；已经预构建前端时可用 `upgrade offline`。

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
通过生成器进入 catalog 后才会出现。

上报节点只保存 delivery 对连接和契约 revision 的引用，转换时主动删除 adapter/mapping/request/
success 的冗余副本；完整发送定义来自版本化契约。这一边界不能破坏，否则 Web 预览、C++ outbox 和
Python worker 会各持一份不同协议。

## 实时流、日志和终端

- CRT 复古外观仅在“终端命令行”页内生效；登录、侧栏、配置画布及其余页面使用原来的设计。
  已撤下整站显示控件、配色状态和效果覆盖层，旧 `console-display-v1` 偏好不再读取。
  终端的主题和显示调校独立保存在 `terminal-appearance-v1`，切换主题不会影响其他页面。

- `stream.py` 只接受 H264/AVC RTSP，GStreamer 零转码封装为 fMP4；前端用 MSE 解析真实 avc codec。
- 同 App 新流会终止旧 GStreamer session，页面重连代码必须避免并发重试风暴。
- App 日志由 `systemd-run --pipe` 读入内存 ring；`/api/apps/{name}/log` 取 tail，
  `/ws/logs/{name}` 推送。
- Terminal 后端为每条 `/ws/terminal` 建 PTY 登录 shell；前端模块级 session registry 在路由切换时
  保留会话，关闭/注销时显式释放。前端 `MAX_TERMINALS=4` 只约束一个前端运行实例，后端路由没有
  实现设备级 PTY 总数上限；若对外开放控制台，容量/安全限制需在后端另行设计。
- 终端工具栏的“全屏”使用 Fullscreen API，仅展开终端网格（包括多分屏及 CRT 玻璃/反射），隐藏侧栏和工具栏；
  悬浮退出按钮或捕获阶段的 Esc 恢复（避免作为终端按键发送给 shell），保留各会话、滚屏和当前字号。ResizeObserver 自动重排字符格并同步 PTY/光学尺寸。
  字号支持直接输入 8–128 CSS px（含小数），有效输入实时应用并保存，失焦/回车规范范围，空值恢复上一次有效值。
  VT323 的可见字形高度小于字体 em 尺寸，例如 20px 的 M 墨迹约 11px；字号按标准 CSS px 应用，不以某个字形的墨迹高度换算。
- 终端外观在 `pages/terminalAppearance.ts` 定义：默认经典琥珀，可切换荧光绿、白磷屏、现代原色。
  “显示调校”可开关 CRT 并调节辉光 Glow、光晕扩散 Bloom、亮度闪烁 Flicker、文字抖动 Jitter、水平同步干扰、RGB 色散、像素点阵及尺寸、扫描线、暗角、雪花质感、边缘畸变、边框反射、荧光残影和动态扫光；外观保存在浏览器
  `terminal-appearance-v1`，字号仍使用 `terminal-fontSize`。配色直接更新已有 xterm 实例，
  不重建 WebSocket/PTY；ANSI 颜色保持可区分。CSS 覆盖层不接收鼠标事件，动态扫光默认关闭，
  并遵循 `prefers-reduced-motion`。视觉灵感来自
  [cool-retro-term](https://github.com/Swordfish90/cool-retro-term)，使用独立的浏览器实现，
  未引入其 Qt/QML 或着色器代码。VT323 从 Google Fonts 原样打包；Departure Mono v1.500 WOFF2 从作者官网原样打包，两者保留各自的 SIL OFL 1.1 声明。旧默认 VT323 首次迁移为 Departure Mono，`fontPreset` 标记确保随后手动切回 VT323 会被保留。
  `TerminalCurvature.tsx` 仅对各终端的文字、光标、选区和效果层做 SVG 桶形畸变，默认强度 45%，
  设为 0 或关闭 CRT 时恢复平面；旧偏好缺失该值时自动补齐。鼠标拖选和终端应用的鼠标坐标按同一映射校正。
  位移贴图只在窗格尺寸变化时生成。弧面露出的区域由 `TerminalBezel.tsx` 绘制成四段拼接的深色塑料内沿；四角有斜接暗槽和细斜面高光，
  各段分别着色形成深浅；玻璃相对前开口四周内收 14 CSS 像素，露出更长内壁以表现整体后移，曲率独立保持原值。反射按前开口尺寸及内收后的玻璃边界采样，关闭畸变时取消内收。`terminalBezelGeometry.ts` 给 SVG 材质与 Canvas 反射提供相同的分界：
  上下两段纵向镜像，左右两段横向镜像；沿内壁深度分带，按玻璃同一交叉轴映射采样，越界裁空而非折返，避免四角重复/反向文字。接缝由外角连到实际弧面玻璃角点（包含屏幕后退量），材质和反射共享分区，接缝处扣除光照，
  并在模糊反射上方覆盖槽线，避免四角出现交叠或跨缝反光；文字、光标和衰减缓存均使用同一处理。
  几何随尺寸变化，关闭畸变后移除拼接内沿。已移除额外的显示器机壳、
  厚斜面、底部散热槽、铭牌和装饰指示灯，保留文字与光标残影。
- `terminalPhosphor.ts` 参考 cool-retro-term 的
  [`terminal_dynamic.frag`](https://github.com/Swordfish90/cool-retro-term/blob/1394ce82fa53d2a87d5adc4d99f9eeba598ea53d/app/shaders/terminal_dynamic.frag)、
  [`terminal_static.frag`](https://github.com/Swordfish90/cool-retro-term/blob/1394ce82fa53d2a87d5adc4d99f9eeba598ea53d/app/shaders/terminal_static.frag)
  和 [`BurnInEffect.qml`](https://github.com/Swordfish90/cool-retro-term/blob/1394ce82fa53d2a87d5adc4d99f9eeba598ea53d/app/qml/BurnInEffect.qml)
  的移动噪声、边缘镜像采样和历史亮度衰减思路，独立用 Canvas/CSS 实现：
  - “像素点阵”默认 70%，尺寸默认 3 CSS px，可选 2–6px；由 `terminalOptics.ts` 生成并缓存
    超采样的圆形荧光点遮光纹理，亮区保留、点间压暗。点阵固定在荧光屏上，文字/光标/残影在其下
    移动，并随整体桶形畸变弯曲；设为 0 或关闭 CRT 时关闭。静态纹理不新增动画或逐帧采样循环。
    可见覆盖层与反射的空间遮光使用同一纹理、尺寸、起点和强度，调整时重建反射 mask，保留会话及残影；
    减少动态效果保留静态点阵。旧偏好自动补齐参数，字号与像素尺寸相互独立。
  - `terminalFonts.ts` 通过 Vite glob 扫描 `src/assets/fonts/` 的一级字体目录；目录名就是下拉框显示名称，
    `font:<目录名>` 为稳定选项标识，原 `departure` / `vt323` 偏好自动迁移。支持目录内（含子目录）的
    WOFF2/WOFF/TTF/OTF 文件，优先常规字形和压缩格式；自动关联 OFL/LICENSE/LICENCE/COPYING 的 TXT/MD 或无扩展名许可。
    新增字体无需维护 TS 列表或 CSS @font-face，只需加入目录并重新构建部署。开发模式由 Vite 自动发现。
    FontFace API 使用独立的终端字体族名称，避免目录名 Arial/Consolas 等覆盖其他页面字体；按所选字体注册和加载，目录可用中文及空格，内部字体名称与文件名称均不限制显示名称；
    只打包资源 URL，不在页面启动时加载整个字体库。空目录/仅说明文件目录不进入选项，删除/改名后偏好回退到默认。
    具体结构与步骤见 `src/assets/fonts/README.md`。默认 Departure Mono（11px 网格，可选 22/33/44px），
    保留系统“现代等宽”。各字体放在自己的目录并携带原作者许可，Vite 输出哈希资源，离线可用。
    中文/缺失字形回退系统字体。等待加载完成再切换 xterm 字体，fit 后同步 PTY 行列数，并发出
    `terminal-font-ready` 让残影/反射重新测量；不重建 WebSocket。
  - `TerminalSignal.tsx` 的 Horizontal Sync 默认 20%，短暂干扰期间最多 10 Hz 更新一张 1×128 的
    横向位移图，各高度的相位不同；幅度上限 8px，间隔 0.9–2.8 秒，避免把同步干扰变成持续整体平移。
    RGB Shift 默认 20%，SVG 分离 R/G/B 后将红/蓝各偏移最多 2.5px 再相加，作用于文字、光标及残影。
    `terminalSignal.ts` 共享实际量化后的坐标偏移，鼠标先经过桶形校正，再加同步偏移；拖选期间保持
    当前波形，松手后恢复。隐藏/减少动态效果停止波形并归零，RGB 可保留静态色散；设为 0 独立关闭。
    反射在镜像前使用同样的位移和颜色通道错位，复用受限分辨率缓冲，不使用逐帧像素读回；信号变化
    只更新反射缓存，不伪造文字更新/残影。颜色分离用同文档 SVG color matrix，保留抗锯齿像素的
    alpha；按源画面版本缓存颜色通道，只有位移变化时复用颜色结果。原有暗角、扫描线和 Flicker 的亮度衰减继续生效。
  - Bloom 默认 35%，在 Glow 字边辉光之外增加两层更宽的文字光晕；`terminalLightHalos()` 给 DOM text-shadow
    与反射采样共享半径、颜色及强度。只采样变化行，不对整张画布每帧执行模糊。
  - Flicker 默认 30%，以非均匀离散相位改变荧光画面及反射容器的透明度；两者共享同一 CSS 动画，
    最大调节对应 16% 亮度下降，保留已有暗角/扫描线的空间遮光。玻璃轮廓与塑料内沿保持固定。
  - Jitter 默认 35%，仅对文字、光标、选区及残影共同施加亚像素位移；100% 时横向最大 0.85px、
    纵向最大 0.35px。采样以该内容容器为坐标原点，避免将视觉抖动误判成光标移动、持续产生伪残影。
    Jitter 与水平同步共用最高 10 Hz 的时钟、位移和帧编号；在同一个任务中先重算镜像，再提交可见
    文字 transform，浏览器绘制前完成两者更新。反射中的文字、光标和残影共享相同位移，不能单独播放
    近似动画。鼠标拖选期间保持当前位移，隐藏/减少动态效果时归零；不改变终端行列数或 PTY 会话。
    旧偏好自动补齐新参数；各滑块设为 0 可独立关闭。隐藏时暂停，减少动态效果时停止抖动/闪烁/动态雪花。
  - 字体亮度范围 0–200%，默认 100%；单独的发光内容层调节文字、光标与残影，玻璃材质/噪声不随之增亮。
    四边反射使用同一亮度系数，在暗角/扫描线/点阵遮光前应用；缓存与实时反射同步变化，保留会话。
  - 雪花复用一张生成的 256×256 噪声纹理，离散移动形成动态颗粒；初始强度 22%，已有偏好保留。
  - 从 xterm 公共 buffer API 读取可见文字及 ANSI/RGB 颜色，镜像到塑料边框；反射强度默认 55%。
  - `terminalOptics.ts` 与可见屏幕共享暗角起点和强度、扫描线透明度、像素点阵遮光、残影透明度。按原屏幕坐标生成椭圆 farthest-corner 暗角及扫描线遮光，
    经同一镜像和接缝映射缓存为低分辨率光照掩码，再由静态 CSS mask 应用于反射；各位置按自身亮度
    衰减，不是整条反射统一乘一个系数。文字辉光在变化行内按 CSS text-shadow 的颜色和半径绘制，再参与采样；避免对整张画布反复应用模糊滤镜。
    调整暗角、扫描线、像素点阵、辉光或 Bloom 立即重建光照缓存，保留文字/光标残影历史及 WebSocket 会话；衰减缓存
    和实时反射都走同一亮度处理，避免缓存反射恢复成未压暗的亮度。
  - 更新行时缓存旧文字，扣除当前文字，按指数衰减形成清屏、滚动和刷新残影；同一历史层也保留
    光标移动、隐藏和闪烁熄灭前的亮度。光标形状与位置从 xterm DOM 读取，CSS 闪烁通过 Web Animations
    的实际半周期边界唤醒，不依赖文字刷新事件；块状、下划线、竖线及失焦轮廓均按实际形状绘制。
    强度默认 55%，
    对应约 0.95 秒清空历史，滑块范围对应 0.16–1.6 秒；设为 0 禁用残影。
  - 信号位移图由 CPU 编码为无损 RGB PNG，避免动态帧调用 Canvas `toDataURL()` 同步读回 GPU；
    相同波形和抖动位置不重复提交，也不更新 SVG href。随机采样频率、幅度和间歇保持一致。
  - `terminalReflectionRaster.ts` 将原反射切片按相同几何组成两个纹理网格，单次上传并批量绘制；
    Canvas 仍按四片内壁裁切和扣除接缝。无 WebGL、着色器不支持或上下文丢失时沿用 Canvas 切片路径。
    停止输出、残影消退且无同步干扰时，缓存最多六个离散抖动位置的反射；文字/光标/光学参数变化立即失效，
    不冻结动态画面、不降低分辨率或频率，退出页面释放缓存和 WebGL 上下文。
  - xterm onRender 和信号位移变更同步刷新反射，只重画变化的文字行；残影衰减定时器最高 20 Hz。
    文字采样宽度上限 1100 像素，反射输出使用半分辨率。
    静止且残影消退后停止磷光帧更新，闪烁光标在下一个相位切换时唤醒，Jitter/水平同步信号另按需同步触发反射；残影衰减阶段缓存镜像，
    避免每帧重新采样全部内沿。隐藏页面或窗格暂停，切换路由清理监听和缓存，保留终端会话。
    `prefers-reduced-motion` 停止雪花动画并禁用残影，保留静态纹理与文字反射。
  - 浏览器回归位于 `web_console/frontend/tests/test_terminal_display.py`，在构建后运行；使用隔离的
    HTTP/WebSocket 模拟，覆盖真实画布像素的反射颜色、文字与光标残影衰减、CSS 闪烁、雪花变化、四角越界不折返、接缝端点与真实玻璃角点对齐、四边独立颜色反射与亮度实图联动、静止内容下显示参数对反射亮度的联动、Bloom 发光像素、抖动/闪烁同步和暂停、同一位移重复得到相同反射像素、水平同步坐标校正、RGB 彩边像素、本地字体加载/行列重排、点阵实际像素的二维周期及反射亮度恢复以及选择/分屏/页面隔离，
    不替代真实 PTY 后端验收。

## 文件和进程安全

沿用现有约束：App 名禁止路径字符和 `.`/`_` 前缀；归档拒绝绝对路径、`..`、symlink；配置只能写
`assets/` 直属 JSON；资产路由只接受受控分类；systemctl unit/action 来自服务端白名单。涉及包替换、
视觉 App 切换和后台服务绑定时使用同一运行锁，避免在检查与写入之间切换 App。

不要在请求协程直接执行长耗时阻塞工作；现有 subprocess/文件重任务按需要放线程或异步子进程。

终端原生全屏回归需桌面浏览器：设置 `CHROMIUM_HEADED=1` 并提供有效的 `DISPLAY` / `XAUTHORITY`，运行 `test_terminal_native_fullscreen_preserves_sessions`；Chromium 91 无界面模式不完成原生全屏请求。
