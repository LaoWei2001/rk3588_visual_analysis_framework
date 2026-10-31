# 终端功能

此目录集中终端页面、会话连接、外观参数与显示效果。`TerminalPage.tsx` 是路由入口；
`terminalSession.ts` 管理 xterm 和 WebSocket，会话在路由切换时保留，在退出登录时关闭。
`sessionLifecycle.ts` 提供轻量清理入口，使其他页面不必提前加载 xterm。

`terminalReflection*`、`terminalPhosphor` 等文件负责不同显示阶段，并非重复的终端实现。
修改时保留 `frontend/tests/test_terminal*.py` 的传输、显示和几何回归。
