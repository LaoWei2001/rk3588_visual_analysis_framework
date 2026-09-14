# GPIO 电平持久化服务

该服务是 GPIO 电平持久化的独立总开关，负责在系统启动早期恢复由 `gpio_test` 和视觉
主程序保存的 GPIO 输出电平。状态按引脚保存在
`/var/lib/rk3588-gpio/GPIOx_Yz.state`。

- 服务开启：测试程序和视觉程序的输出会保存，开机时会恢复。
- 服务关闭：两者仍可实时控制 GPIO，但不会读取、恢复或更新持久状态。
- 视觉程序启动、停止、重启或切换应用，不会开启、关闭或重启本服务。
- 服务关闭期间的 GPIO 变化不会覆盖最后一次保存的状态；重新开启时恢复最后保存值。

实时输出与重启保持是两个独立层次：`rk3588-gpio-control.service` 是始终运行的底层实时
控制器，它持有已经设置的 GPIO，因此 `gpio_test set 1` 退出后电平不会被外接下拉立即拉回
0；`rk3588-gpio-restore.service` 只是网页可开关的持久化功能。关闭持久化不会停止实时
控制器，也不会妨碍测试程序或视觉程序自由设置 0/1。

## 安装

正常部署无需单独进入本目录。安装 Web 控制台时会自动安装本服务：

```bash
cd /userdata/rk3588_visual_analysis_framework
sudo bash web_console/install.sh online
```

下面的独立入口只用于不安装 Web 控制台的设备或故障修复：

```bash
cd /userdata/rk3588_visual_analysis_framework
sudo ./service/gpio_state/install.sh
```

安装脚本会完成以下工作：

1. 构建 `gpio_test/gpio_test`；
2. 构建并安装实时控制后台 `rk3588_gpio_control_daemon`；
3. 安装并启用始终运行的 `rk3588-gpio-control.service`；
4. 安装 GPIO 持久化开关 `rk3588-gpio-restore.service`；
5. 首次安装时启用状态恢复，并自动将继电器 `GPIO6_A2` 设置、保存为低电平；
6. 升级时保留网页中的原有开关状态和已经保存的 GPIO 电平，不强制改回低电平。

如果其他板卡的继电器不在 `GPIO6_A2`，首次安装时可以指定实际引脚：

```bash
sudo RK3588_RELAY_PIN=GPIO7_A0 ./service/gpio_state/install.sh
```

服务挂载到 `sysinit.target`，并排在声音服务、Web 控制台和视觉程序之前。服务是
`oneshot` 类型，完成恢复后不会独占 GPIO，后续视觉程序仍可正常申请和控制引脚。
它紧跟 udev 设备触发阶段运行，不等待其他本地分区挂载；若 GPIO 控制器尚未生成，
恢复工具会等待最多 2 秒。

安装 Web 控制台后，也可以进入“系统服务”页面操作“GPIO 电平保持服务”：开启会立即
恢复保存电平并启用以后每次开机恢复；关闭会停止服务并取消开机恢复。

服务用 `/run/rk3588-gpio-persistence/enabled` 作为只读运行时开关。该目录由 systemd
随服务创建和删除；视觉程序只检查它，不会修改它。即使视觉程序已经占用 GPIO，开启
服务也不会因此失败：当次无法恢复的引脚会记录到日志，之后视觉程序的输出仍会保存。

## 上电瞬间的硬件安全态

该服务只能在内核创建 `/dev/gpiochipN` 后恢复电平。PCA9555 在上电复位后默认是输入态，
因此从接通电源到内核识别扩展器之间的电平不能由 systemd 或用户程序控制。若外设在这段
时间也必须保持关闭，应在高电平有效的控制端增加合适的下拉电阻（常见起点为 4.7kΩ～10kΩ），
或把驱动电路设计为默认关闭/低电平有效；阻值和接法应结合实际模块输入电路确认。

## 检查

```bash
systemctl status rk3588-gpio-restore.service
systemctl status rk3588-gpio-control.service
journalctl -u rk3588-gpio-restore.service -b
journalctl -u rk3588-gpio-control.service -b
/usr/local/bin/gpio_test get
```
