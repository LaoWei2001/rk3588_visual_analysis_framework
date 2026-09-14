# GPIO 电平设置与重启恢复

`gpio_test` 默认控制 `GPIO6_A0`，也可以通过 `--pin` 操作其他符合
`GPIOx_Yz` 命名规则的引脚。独立的 GPIO 电平保持服务开启时，每次成功设置输出后，
期望电平都会保存到 `/var/lib/rk3588-gpio/`；服务关闭时只改变当前电平，不保存。

安装服务后，测试程序通过始终运行的 `rk3588-gpio-control.service` 设置GPIO。后台进程会
持续持有输出线，因此命令返回后高/低电平仍然保持，不会因测试程序退出而变回硬件默认值。
这项实时控制能力不受“GPIO电平保持服务”开关影响。

## 构建与服务安装

```bash
./build.sh
```

正常部署时，`web_console/install.sh` 会自动安装 GPIO 服务，不需要再进入
`service/gpio_state/`。GPIO 测试工具保留在本目录；首次安装会
启用 `rk3588-gpio-restore.service`；再次安装会保留网页中已有的开启/关闭状态。服务在
系统初始化和声音服务之前恢复所有已保存的 GPIO 输出。

## 使用

```bash
# 列出当前板卡全部 GPIO，并显示占用状态
./build/gpio_test list

# 只列出空闲候选项和已经由本框架控制的 GPIO
./build/gpio_test list --available

# 只查看指定 gpiochip，例如本板 TCA9555 所在的 gpiochip6
./build/gpio_test list --chip 6

# GPIO6_A0 设置为低电平（保持服务开启时同时保存）
sudo ./build/gpio_test set 0

# GPIO6_A0 设置为高电平（命令退出后仍保持）
sudo ./build/gpio_test set 1

# 查询保存值并重新施加到引脚
sudo ./build/gpio_test get

# 指定其他引脚
sudo ./build/gpio_test --pin GPIO6_A2 set 0

# 手动模拟一次开机恢复
sudo ./build/gpio_test restore
```

`list` 是无扰动扫描，不会申请线路或改变电平。每一行会显示动态生成的 `GPIOX_YZ`、
offset、设备树线路名称、当前方向、占用者，以及以下状态：

- `可控-空闲`：当前没有内核或进程占用，是可以进一步核对的候选GPIO。
- `可控-本框架`：已经由实时GPIO后台持有，可继续用本工具或视觉程序设置。
- `禁止-已占用`：由电源、复位、USB、扬声器或其他内核功能使用，不应进行电平测试。

注意：`可控-空闲`只表示软件层面没有占用，不能证明引脚已经引出，也不能证明翻转它在
电气上安全。切换板卡后应重新运行`list`，根据输出中的`gpiochip/标签`和`线路名称`与该板
原理图交叉确认，再对单个引脚执行`set 0/1`；程序不会冒险自动翻转所有空闲GPIO。

不带参数运行时保留原测试程序的翻转行为。正式程序和启动脚本应始终使用明确的
`set 0` 或 `set 1`，不要依赖翻转。

视觉主程序的 `src/gpio/gpio.cpp` 使用同一个独立服务开关和状态目录。视觉程序不能启停
保持服务；服务开启时成功输出会更新状态，服务关闭时则只进行实时控制。
