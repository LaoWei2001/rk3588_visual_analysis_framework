# GPIO 电平持久化服务

该服务是 GPIO 电平持久化的独立总开关，负责在系统启动早期恢复 GPIO 实时控制后台统一
保存的输出电平。`rk3588-gpioctl`、视觉主程序和 GPIO API 测试程序只提交控制请求，不再
各自写状态文件。状态按引脚保存在
`/var/lib/rk3588-gpio/GPIOx_Yz.state`。

- 服务开启：测试程序和视觉程序的输出会保存，开机时会恢复。
- 服务关闭：两者仍可实时控制 GPIO，但不会读取、恢复或更新持久状态。
- 视觉程序启动、停止、重启或切换应用，不会开启、关闭或重启本服务。
- 服务关闭期间的 GPIO 变化不会覆盖最后一次保存的状态；重新开启时恢复最后保存值。

实时输出与重启保持是两个独立层次：`rk3588-gpio-control.service` 是始终运行的底层实时
控制器，它持有已经设置的 GPIO，因此 `rk3588-gpioctl output 1` 退出后电平不会被外接下拉立即拉回
0；`rk3588-gpio-restore.service` 只是网页可开关的持久化功能。关闭持久化不会停止实时
控制器，也不会妨碍测试程序或视觉程序自由设置 0/1。

## 安装

正常部署无需单独进入本目录，使用项目根目录的统一安装入口：

```bash
cd /userdata/rk3588_visual_analysis_framework
sudo ./setup/install.sh online
```

下面的独立入口只用于不安装 Web 控制台的设备或故障修复：

```bash
cd /userdata/rk3588_visual_analysis_framework
sudo ./service/gpio_state/install.sh
```

安装脚本会完成以下工作：

1. 构建并安装正式命令行工具 `rk3588-gpioctl`，并保留旧名称 `gpio_test` 兼容入口；
2. 构建并安装实时控制后台 `rk3588-gpio-daemon`，并保留旧程序名兼容入口；
3. 安装并启用始终运行的 `rk3588-gpio-control.service`；
4. 安装 GPIO 持久化开关 `rk3588-gpio-restore.service`；
5. 首次安装时启用状态恢复，并自动将继电器 `GPIO6_A2` 设置、保存为低电平；
6. 升级时保留网页中的原有开关状态和已经保存的 GPIO 电平，不强制改回低电平。

如果其他板卡的继电器不在 `GPIO6_A2`，首次安装时可以指定实际引脚：

```bash
sudo RK3588_RELAY_PIN=GPIO7_A0 ./setup/install.sh online
```

恢复服务挂载到 `sysinit.target`，并排在声音服务、Web 控制台和视觉程序之前。恢复服务是
`oneshot` 类型；实时控制服务持续运行并统一持有已设置的 GPIO，测试程序和视觉程序通过它
继续修改电平，不会互相争抢线路。
它紧跟 udev 设备触发阶段运行，不等待其他本地分区挂载；若 GPIO 控制器尚未生成，
恢复工具会等待最多 2 秒。

安装 Web 控制台后，也可以进入“系统服务”页面操作“GPIO 电平保持服务”：开启会立即
恢复保存电平并启用以后每次开机恢复；关闭会停止服务并取消开机恢复。

服务用 `/run/rk3588-gpio-persistence/enabled` 作为只读运行时开关。该目录由 systemd
随服务创建和删除；GPIO 后台负责检查它，视觉程序不会修改它。即使视觉程序已经占用 GPIO，开启
服务也不会因此失败：当次无法恢复的引脚会记录到日志，之后视觉程序的输出仍会保存。

新版客户端使用 `SET_MANAGED` 和 `INPUT_MANAGED` 协议：后台在同一个串行请求中完成硬件
方向/电平、运行记录和开机状态更新，因此多个入口不会再出现“当前电平属于后一个请求，
保存值却被前一个请求覆盖”的顺序反转。公共 `gpio_set_output()`、`pin_out_val()`、
`gpio_set_input()` 接口保持不变；旧后台仍可由底层自动兼容，无需修改业务逻辑。

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
/usr/local/bin/rk3588-gpioctl get
/usr/local/bin/rk3588-gpioctl --pin GPIO6_B3 read
```

实时控制服务协议区分三种读取：`STATUS` 只查询服务正在持有的输出，`READ` 只读取已经处于
输入方向的空闲线路，旧的 `GET` 保留给现有视觉程序和兼容调用。这样命令行 `get` 不会再
被误当作输入采样，`read` 也不会把正在控制设备的输出线改成输入。

命令行还可以显式切换方向：

```bash
/usr/local/bin/rk3588-gpioctl --pin GPIO6_B3 input
/usr/local/bin/rk3588-gpioctl --pin GPIO6_B3 output 0
```

后台的 `INPUT_MANAGED` 请求会持续持有输入线路，并用运行时 `.input` 标记在后台重启后恢复；
`SET_MANAGED` 既负责切换成输出、设置电平，也负责按服务开关统一提交保存值。输出方向必须带
明确初始值。持久化开启时，切成输入会在同一个请求内删除
该引脚旧的 `.state` 输出恢复值，避免下次启动又恢复成输出。任何已经由内核或其他进程占用
的线路都会返回忙，不会被强制接管。
