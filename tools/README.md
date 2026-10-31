# 仓库工具

本目录保存跨越引擎与业务工作区的开发、生成和部署工具，不放运行时业务代码。

```text
tools/
├── check.py                   # 审核整改的代表性回归入口（不安装、不部署）
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

在依赖已经安装的环境执行 `python3 tools/check.py`，依次检查后端关键回归、
前端构建/传输与终端算法回归、C++构建及非硬件CTest。它是代表性回归集，
不替代各模块的完整测试。用 `--suite browser` 单独运行终端连接、终端显示、
实时日志和配置同步浏览器回归；浏览器需要 Playwright 和 Chromium，
可通过 `CHROMIUM_PATH` 指定浏览器。`--suite` 支持选择多项，`--dry-run` 仅列出命令。

硬件推理测试带 `hardware` 标签，需准备模型并开启相应 CMake 选项后，
在构建目录手动执行 `ctest --output-on-failure -L hardware`。硬件测试会占用 NPU。
