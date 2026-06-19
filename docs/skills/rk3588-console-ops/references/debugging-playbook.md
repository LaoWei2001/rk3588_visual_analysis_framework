# 调试手册(debugging playbook)

出问题时**怎么定位**——日志在哪、怎么二分、怎么排除"改了不生效",以及一个完整实战案例(Web 终端)。预防性的"坑"在各 SKILL.md 和其它 reference 里;本篇讲**当它不工作时,如何一步步逼出根因**。

## 〇、黄金第一步:先排除"改了根本没生效"

本项目调试**最大的时间黑洞**:你在开发机改了代码,但板子跑的还是旧的,于是对着没变的现象反复猜。**任何"改了还是老样子"的问题,先过这张清单:**

| 你改的                               | 必须做什么才生效                                                                  | 确认真生效的方式                                                         |
| --------------------------------- | ------------------------------------------------------------------------- | ---------------------------------------------------------------- |
| 前端 `src/*`                        | `install.sh`(重 build)+ 浏览器 **Ctrl+Shift+R**                               | DevTools→Network 看 `index-xxxx.js` 文件名变了;或加一句临时 `console.log` 看到 |
| 后端 `backend/*`                    | `install.sh` 或 `systemctl restart rk3588-console`                         | `journalctl` 里进程 **PID 变了**;或加一条启动日志看到                           |
| C++ 逻辑/上报/源码                      | `cd rk3588_yolo && ./build.sh <名> && sudo ./install_app.sh <名>` → 网页重启该程序 | 监看画面行为变化;overlay                                                 |
| `config.json` 普通字段                | 网页保存即可(C++ 热重载)                                                           | 下一帧生效                                                            |
| **ROI**(roi_zones.json)           | **停止再启动**该程序(不热重载)                                                        | 重新画的区域生效                                                         |
| 服务配置(config.yaml/ota_config.json) | 把对应后台服务**停止再启动**                                                          | `journalctl -u <svc>`                                            |
| 开发机改了文件                           | **先 `scp` 到板子**再 install(板子跑的是 `/opt/ai_apps/_console`,不是你的开发副本)          | —                                                                |

> 经验法则:现象**一模一样、像素级没变**,99% 是没生效,而不是你的修法错了。先证明新代码在跑,再谈对错。

## 一、各部分日志在哪

```bash
# Web 控制台(FastAPI 后端 + 它托管的前端)
journalctl -u rk3588-console -f                 # 实时
journalctl -u rk3588-console -n 80 --no-pager   # 最近 80 行
systemctl status rk3588-console --no-pager      # 起没起来 / 崩溃原因

# 两个后台微服务
journalctl -u ota_agent -f
journalctl -u unified_upload -n 50 --no-pager

# C++ 推理二进制:由控制台 process_manager 管，网页「程序管理」看日志，
# 或后端接口 /api/apps/{name}/log;裸跑调试见 services 文档

# 前端(浏览器侧):F12 → Console(JS 报错) / Network(看 /api 响应、WS 帧)
```

- **后端临时日志**:在 Python 里 `log.warning(...)`(`log = logging.getLogger(...)`),冒在 `journalctl -u rk3588-console`。uvicorn 默认还会打每条 HTTP 访问日志(`GET /api/... 200`)——前端每 5s 轮询 `/api/apps`、`/api/services` 属正常,不是 bug。
- **前端临时日志**:`console.log(...)` → DevTools Console。要看 WS 收发的原始字节,Network → WS → 选中连接 → Messages。
- ⚠️ **临时日志用完即删**——尤其**别长期记录终端按键**(会把密码写进 journald)。

## 二、二分定位法:先确定是哪一层

一个"功能不工作",几乎都能用"输入有没有进去 / 输出有没有出来 / 有没有渲染"把战场劈成两半:

```
用户操作 → 前端 → (WS/HTTP) → 后端 → 板端(进程/PTY/系统) → 后端 → 前端 → 渲染
            └── 在每个箭头加一条日志,看消息走到哪一步断了 ──┘
```

- **前端发出去了吗**:DevTools Network/Console,或后端入口日志。
- **后端收到了吗 / 转发了吗**:后端 `log.warning` 在收/发处各打一条。
- **板端真做了吗**:`ps`/`pgrep`/`systemctl`/对应服务日志。
- **回到前端渲染了吗**:DevTools Console 有没有抛异常(渲染崩了);Network 有没有收到响应/帧。

> 关键心态:**别从中间猜,从两端往中间夹**。先证明"字节到了 X 层",再看"X 层为什么没处理对"。下面的终端案例就是这么破的。

## 三、实战案例:Web 终端(xterm.js ⇄ PTY)

这是一个"多个独立 bug 叠在一起"的典型,定位过程值得复用。架构:

```
浏览器 xterm.js (前端 TerminalPage / terminalSession.ts)
   │  WebSocket /ws/terminal?token=...   (二进制收 PTY 输出 / 文本发按键、resize)
   ▼
后端 routers/terminal.py:  pty.fork() → bash -l
   ├ loop.add_reader(master_fd): 读 PTY → 队列 → websocket.send_bytes
   ├ ws→pty: 收到文本/字节 → os.write(master_fd);  "\x1b[resize:RxC" → TIOCSWINSZ + SIGWINCH
   └ 断开收尾: 关 master(SIGHUP 前台组) → SIGHUP bash → SIGKILL 进程组 → waitpid
```

逐个 bug 和它的定位/修法(都已落到代码,改坏时按此回溯):

1. **vim 一打开就"卡死",但 top/htop 正常**
   
   - 真相不是卡死,是**渲染**问题。用后端日志证明 vim 的输出(连 `-- 插入 --`、`:q!` 退出后的 shell 提示符)**都发给了浏览器**,于是定位到前端没画出来。
   
   - 子因 A:**PTY 尺寸与可视区不匹配**。后端给了固定 40 行,前端实际可见不足 40 行 → vim 把状态栏/底部画到屏幕外。修:前端用 `ResizeObserver` 在容器尺寸稳定/变化时 `fit()`,把 `term.rows×term.cols` 用 `\x1b[resize:` 同步给后端(`TIOCSWINSZ`+`SIGWINCH`),并忽略 0/非法尺寸。
   
   - 子因 B(根因):**xterm.js 6.0.0 压缩产物的 DECRQM bug**。vim 启动发 `ESC[?12$p`(请求模式查询),xterm 的 `requestMode` 抛 `ReferenceError: r is not defined`,异常发生在写入主循环 `_innerWrite` 里,**打断整个渲染管线**——开场白(崩前画的)定格,之后全不刷新。**靠 DevTools Console 的红色报错一眼定位**。修:在 xterm 解析器抢先拦截 DECRQM,`term.parser.registerCsiHandler({intermediates:'$',final:'p'},()=>true)` 和 `{prefix:'?',intermediates:'$',final:'p'}`,让那个会崩的内置处理器不被调用。更彻底可降到 `@xterm/xterm@5.5.0`。

2. **`TERM` 决定 vim 会不会发这些探测**:`xterm-256color` 会发 DA/CPR/OSC/DECRQM(于是踩中上面的崩);`linux`(板子 HDMI/串口控制台默认)不发,所以 `TERM=linux vim` 能用、`top`/`htop` 也从不触发。**别用换 TERM 来"绕过"**——要和板端 SSH 一致就保持 `xterm-256color`,把 xterm 的 bug 修掉。

3. **按键没反应 / 怎么定位输入**:在后端 `ws→pty` 处临时 `log.warning` 打印收到的文本,双击/打字时看 `journalctl`。能看到 `'i'`、`'\r'` 就证明"键到了 PTY",问题在 vim/渲染;看不到就是前端没发(焦点/onData)。

4. **双击/选区松手冒出 `^C`**:`^C` = 字节 `\x03`。根因是**搜狗等中文输入法的"划词/选中即复制"在鼠标动作后合成了一次 Ctrl+C**,被 xterm 当 SIGINT 发出。修:`attachCustomKeyEventHandler` 里,Ctrl+C 满足"有选区 / `!isTrusted`(合成) / 250ms 内刚有鼠标交互"任一 → 当作复制并拦截,**不发 `\x03`**;只有纯键盘且无选区才作为中断。

5. **Ctrl+V 没反应**:xterm 默认把 Ctrl+V 当控制字符 `\x16`(literal-next)发出去并拦掉事件,导致浏览器原生 `paste` 事件不触发。修:自定义按键处理器对普通 Ctrl+V `return false`,放行给浏览器原生粘贴(xterm 监听了 `paste` 事件,HTTP 下也能用)。

6. **复制粘贴不如 VSCode/板端顺手**:根因是板子是 **HTTP(非安全上下文)**,浏览器**禁用剪贴板 API**(`navigator.clipboard` 不可用)。修:复制用 `document.execCommand('copy')` 兜底;粘贴靠原生 `Ctrl+V`。**右键粘贴、`Ctrl+Shift+V`、`Shift+Insert`(需程序主动读剪贴板)只有 HTTPS 才行**。要完整体验给控制台配 HTTPS。

7. **进程会不会堆积**:每个 WS = 一个独立 PTY(`pty.fork` 已 setsid,pid 即进程组长)。断开收尾:`os.close(master)` 给前台进程组发 SIGHUP(vim/top 退出)→ `kill(pid,SIGHUP)` 让 bash 挂断作业 → `killpg(pid,SIGKILL)` 兜底 → `waitpid` 回收。验证:反复刷新后 `pgrep -af 'bash -l'` 应始终最多 1 个(分屏时为分屏数),不递增;开着 vim 刷新后 `pgrep -af '[v]im'` 应为空。

> 这个案例的通用教训:**先用后端日志证明"数据已送达浏览器",把问题锁死在前端;再用 DevTools Console 的异常锁死到具体那行/那个库**。多 bug 叠加时,一次只证伪一个假设。

## 四、其它常见症结速查

- 后台服务起不来 `CHDIR / Failed at step CHDIR`、单元路径失效、网页与命令行如何配合 → `services-upload-and-ota.md` §7。
- USB ROI 偏移、改 ROI 不生效、OTA 换模型没生效、apt/Node 国内网络 → `SKILL.md` 四。
- 给逻辑加参数后网页没有输入框 / 改了值不生效 → `rk3588-channel-logic` 的 `adding-config-parameter.md`(四处 key 对齐 + 热重载约定)。
- 前端"改了不生效"、加页面/加接口、WS 用法 → `web-console-frontend.md`。
