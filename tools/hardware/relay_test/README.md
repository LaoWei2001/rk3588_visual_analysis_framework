# 继电器独立测试

`relay_test` 是 `rk3588-gpioctl` 的继电器专用入口，默认操作 `GPIO6_A2`。它不再直接申请
libgpiod 线路，而是把命令交给统一的实时 GPIO 控制后台，因此不会与 GPIO 命令行工具或视觉
主程序发生 `Device or resource busy` 冲突。

正常部署使用项目根目录的一条命令，它会同时安装 Web 控制台、实时控制和电平保持服务：

```bash
cd /userdata/rk3588_visual_analysis_framework
sudo ./rkvision platform install online
```

`service/gpio_state/install.sh` 只保留为内部组件安装和故障修复入口。

构建和测试：

```bash
cd /userdata/rk3588_visual_analysis_framework/tools/hardware/relay_test
./build.sh

./build/relay_test get
./build/relay_test output 1
./build/relay_test output 0
```

不带参数自动翻转、`set` 和 `mode` 命令都已经移除。必须明确使用 `output 0` 或
`output 1`，避免误操作继电器。

切换板卡后，可以指定从 `rk3588-gpioctl list` 和原理图确认的新引脚：

```bash
./build/relay_test --pin GPIO7_A0 get
./build/relay_test --pin GPIO7_A0 output 1
./build/relay_test --pin GPIO7_A0 output 0
```

GPIO 电平保持服务开启时，设置值会保存并在下次启动恢复；服务关闭时只控制本次运行的
电平。实时控制能力在两种情况下都可用。
