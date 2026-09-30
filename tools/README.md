# 仓库工具

本目录保存跨越引擎与业务工作区的开发、生成和部署工具，不放运行时业务代码。

```text
tools/
├── build/                     # Logic 清单与上报模板生成器
├── device/                    # 在设备上独立运行的配置与诊断工具
│   ├── first_net_config/      # 首次网络配置终端工具
│   └── gpio_test/             # GPIO 控制与测试工具
└── offline_dev_install/       # 按目标发行版隔离的离线开发环境与制包工具
    ├── debian/
    └── ubuntu/
```

应用构建的唯一公开入口是仓库根目录 `build.sh`。CMake 中间产物统一写入
`build/engine/`，完整应用包统一写入 `dist/<app>/`。

`device/` 中的程序拥有各自的 `build.sh`，不会参与主引擎的默认构建。`offline_dev_install/`
中的脚本应从仓库根目录调用，其输出分别保存在对应发行版目录的 `output/` 中。
