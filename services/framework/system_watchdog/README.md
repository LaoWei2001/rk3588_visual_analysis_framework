# 系统硬件看门狗与定时重启

本目录还提供独立的内核 D 状态故障升级监测。完整的应用重启、业务超时、自启条件、
限频与验收说明见[无人值守恢复](../../../docs/unattended-recovery.md)。正常通过根安装器集成；
独立平台安装入口是 `sudo python3 services/framework/runtime_manager/install_runtime.py`。
硬件接管仍使用本目录的 `status/apply/ensure`；运行策略由平台库和 Web 管理入口共享。

每日重启与死机恢复使用两条独立链路：Web 每日重启配置生成 systemd timer；运行时硬件
看门狗由 PID 1（systemd）持有设备并定期发送心跳，不依赖浏览器、Web 后端或视觉程序。

## EASY EAI Monster 板端配置

2026-10-15 检查的设备运行 Linux 5.10.160 和 systemd 247，存在：

| 设备 | 驱动 | 说明 |
| --- | --- | --- |
| `/dev/watchdog0` | `gpio-wdt` | 设备树配置 `hw_margin_ms=1600`、`always-running`；本次故障恢复实测未通过 |
| `/dev/watchdog1` | `dw_wdt` | 官方例程实际使用的 RK3588 DesignWare 看门狗；当前优先选择，本板受控内核崩溃复位验收通过 |

用户提供的官方例程为仓库根目录 `watchdog_demo/test-watchDog/main.c`。它打开 `/dev/watchdog1`，
用 `WDIOC_SETOPTIONS/WDIOS_ENABLECARD` 启用、`WDIOC_SETTIMEOUT` 配置、`WDIOC_KEEPALIVE`
喂狗。systemd 247 的内置看门狗使用相同 Linux 接口，无需把示例循环塞入 Web 或视觉程序。
例程在喂狗 6 次后主动停止喂狗以测试复位，不要直接当作生产守护运行。

原配置优先选择 GPIO 是不适合本板的默认策略：实测能看到 GPIO 翻转，但注入内核故障后等待
约 5 分钟仍未自行恢复，用户最终人工断电。尚无证据认定该 GPIO 在本盒子上实际接通可靠复位链路，
也不能仅凭驱动名认定为已验证的外部看门狗。更换为官方使用的 DW 后，本板已完成独立验收：
临时将 `kernel.panic` 设为 0 排除软件重启，注入内核崩溃，记录估计约 89.7 秒后进入新内核
启动，用户确认无需人工干预便自行恢复，SSH 和 Web 随后可用。开机后 panic 超时恢复为 10 秒。
最终记录见本机 `/var/lib/rk3588-watchdog-validation/dw-reset-reviewed.json`，Web 显示本板
验收通过。其他设备或更换内核后仍需独立验收；本次结果不代表所有 SSH 失联原因都已覆盖。

设备编号可能随 BSP 变化；安装器检查设备对应驱动，升级 BSP 后应重新核对。
以下只读检查不会打开看门狗设备，也不会触发计时：

```bash
python3 services/framework/system_watchdog/install_watchdog.py status
```

首次启用且 systemd 尚未持有其他看门狗时，按本板实际驱动指定设备：

```bash
sudo python3 services/framework/system_watchdog/install_watchdog.py apply --device /dev/watchdog1 --timeout 60
```

安装器写入 `/etc/systemd/system.conf.d/90-rk3588-watchdog.conf`，首次接管或同设备修改超时通过
`daemon-reexec` 应用，检查 PID 1 已持有该设备，并报告运行时实际超时。失败时恢复原配置。
配置独立于应用包，整机启动时由 systemd 读取。无需运行单独的喂狗程序。

正常升级使用独立平台安装器。它发现旧项目配置使用 GPIO、DW 空闲时，只准备下次正常启动的
DW 配置并返回 `reboot_required=true`，不在线切换设备；当前 GPIO 继续喂狗。必须正常重启后
核对 PID 1 持有的是 DW。完成重启前不要手动执行 `daemon-reexec`；管理员自定义配置不自动覆盖。

DW 超时存在硬件档位，例程说明请求 45～89 秒时实际约 89 秒。因此请求 60 秒不等于实际
60 秒，必须读取 `systemctl show -p RuntimeWatchdogUSec -p RebootWatchdogUSec` 的返回值。
超时按驱动返回，不根据表格自行换算或承诺所有卡死都在严格时间上界内恢复。

`RuntimeWatchdogSec` 保护运行阶段；`RebootWatchdogSec` 保护正常服务终止后、进入
systemd-shutdown 的重启阶段。后者不等于从发出 reboot 命令开始整个流程最多 60 秒。

GPIO 设备树中的 `hw_margin_ms=1600` 只表明驱动预计的喂狗间隔，不能证明实物接线、器件使能
或复位有效。它不能作为本盒子已通过硬件保护验收的证据。

平台安装另外启用 `/etc/sysctl.d/90-rk3588-recovery.conf` 中的 `kernel.panic=10`，即时生效并
随系统启动加载。已有非零管理员值保留。内核进入 panic 后仍能执行重启代码时按此等待重启；
彻底锁死不能依靠该软件路径。单独启用而不重启设备可使用：

```bash
sudo python3 services/framework/system_watchdog/install_watchdog.py panic-recovery
```

不会自动把 OOM、普通警告或 D 状态变成 panic。验证 DW 硬件复位时需排除软件 panic 重启，
否则仅能说明软件兜底工作。Web 上接管和验收结论分别展示，人工断电不算自动恢复成功。

PID 1 正常运行但视觉程序单独卡死时，systemd 仍会喂系统看门狗。应用失活应通过独立的
业务心跳检查及服务恢复处理，不能依靠运行时系统看门狗判断视频是否仍有有效推理。

## 每日固定时刻与整机死机

systemd timer 只在 Linux 能执行任务时提供日历调度。硬件看门狗能恢复失去心跳的系统，
但自身不理解“每天几点”。这两个机制组合后仍不能证明整机已死机时会在指定钟点复位。

官方 RTC 电路采用 PCF85063A，文档描述 `RTC_INT_L` 为中断输出，没有确认该输出直接控制
复位或电源。本机设备树为 `nxp,pcf85063`，未暴露 RTC wakealarm；即使补齐 RTC 中断驱动，
仍需要独立的硬件复位链路，不能靠已挂起的 Linux 收到中断后再执行 reboot。

严格的固定钟点复位需要始终供电、独立计时并直接控制 RESET/电源的控制器，例如外部 MCU
配 RTC，或有每日计划的工业电源控制模块。确认接线、实际板卡版本和模块协议后，再将 Web
时间设置同步到该控制器。没有实际控制器时，界面不能显示“硬件定时复位已生效”。

## 验收边界

配置检查、PID 1 持有设备及持续运行只能验证接管与心跳链路。现场还需在维护时间分别测试
用户态失去心跳、内核停止运行、重启过程停滞、断电恢复和指定钟点复位，记录复位及业务恢复
时间。停止喂狗或制造内核故障会实际重启整机，不能作为无中断检查运行。

官方依据：

- [EASY EAI 看门狗](https://www.easy-eai.com/document_details/18/531)
- [EASY EAI RTC 电路](https://www.easy-eai.com/document_details/18/581)
- [systemd 247 管理器看门狗配置](https://github.com/systemd/systemd/blob/v247/man/systemd-system.conf.xml)
- [Linux 看门狗驱动接口](https://docs.kernel.org/watchdog/watchdog-api.html)
