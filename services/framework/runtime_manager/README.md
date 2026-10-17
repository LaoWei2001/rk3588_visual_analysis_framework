# 独立设备运行平台

Web 管理入口为侧边栏“故障恢复”，内分系统、网络与程序故障恢复。实际运行由 systemd 管理：

- `rk3588-runtime-manager.service`：按用户自启意图在开机时启动应用并协调上传/OTA 服务；
- 视觉应用的 transient service：负责进程归属、正常启停和日志，默认不因退出或卡顿重启；
- `rk3588-recovery.timer/service`：兼容保留的内核阻塞升级机制，默认关闭升级重启；
- `rk3588-network-recovery.service`：按独立网络策略巡检有线/Wi-Fi 管理出口并逐级恢复，策略默认关闭；
- systemd 管理器：独立持有硬件看门狗。

应用退出重启与业务心跳检测默认关闭，可在“程序故障恢复”分类分别开启，见
[程序恢复说明](../../../docs/application-recovery.md)。保存不自动重启程序，立即应用由用户明确操作。
开机自启仍在程序管理页单独选择，系统和网络恢复与程序恢复分别设置。

这里的“底层运行”指系统级用户态服务，不是在 Web 内建立定时线程。平台使用 Python 标准库，
不需要 FastAPI、Uvicorn、React 或 Web 目录。共享运行库同时供 Web 管理客户端使用，跨进程
运行锁和状态文件使网页操作与系统协调保持一致。

完整安装使用根目录 `sudo ./install.sh offline`。只安装平台、保留现有 Web 时可执行：

```bash
sudo python3 services/framework/runtime_manager/install_runtime.py
```

这会把库安装到 `/usr/lib/vision-analysis-platform/python`，安装并启用系统服务；不会替用户
开启原来关闭的应用自启。离线制包使用 `--stage <目录>` 只生成文件，平台单独封装成
`vision-analysis-platform` deb，控制台包声明依赖。移除 Web 包不会停止平台。

硬件看门狗优先按官方 `watchdog_demo` 选择 `dw_wdt`。旧 GPIO 接管迁移会提示需要正常重启，
不会在线关闭旧计时器；重启前不要执行 `daemon-reexec`。安装同时启用默认 10 秒的内核 panic
重启兜底，保留已有非零超时。用户在“系统故障恢复”保存过独立开关后，安装/升级保留用户选择，
不自动重新开启，也不在线切换硬件。硬件开关正常重启后生效，panic 开关保存后立即生效。
开关持久配置由系统读取，不依赖 Web 存活。Web 将接管状态与实机复位验收结果分开显示。

系统配置、参数生效方式、恢复边界和验证命令见[无人值守恢复说明](../../../docs/unattended-recovery.md)。
网卡选择、用户探测目标、持久限频和网络事务协调见[网络失联自动恢复](../../../docs/network-recovery.md)。
