# 继电器独立测试

`relay_test` 是 `gpio_test` 的继电器专用入口，默认操作 `GPIO6_A2`。它不再直接申请
libgpiod 线路，而是把命令交给统一的实时 GPIO 控制后台，因此不会与 `gpio_test` 或视觉
主程序发生 `Device or resource busy` 冲突。

正常部署时，`web_console/install.sh` 会自动安装实时控制和电平保持服务，不需要单独进入
`service/gpio_state/`。如果设备不安装 Web 控制台，才使用独立安装入口：

```bash
cd /userdata/rk3588_visual_analysis_framework
sudo ./service/gpio_state/install.sh
```

构建和测试：

```bash
cd /userdata/rk3588_visual_analysis_framework/relay_test
./build.sh

./build/relay_test get
./build/relay_test set 1
./build/relay_test set 0

# 不带参数时切换当前状态
./build/relay_test
```

切换板卡后，可以指定从 `gpio_test list` 和原理图确认的新引脚：

```bash
./build/relay_test --pin GPIO7_A0 get
./build/relay_test --pin GPIO7_A0 set 1
./build/relay_test --pin GPIO7_A0 set 0
```

GPIO 电平保持服务开启时，设置值会保存并在下次启动恢复；服务关闭时只控制本次运行的
电平。实时控制能力在两种情况下都可用。
