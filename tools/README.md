# 仓库工具

本目录保存跨越引擎与业务工作区的开发、生成和部署工具，不放运行时业务代码。

## 先选要做的事

| 你想做什么 | 打开哪份说明 |
| --- | --- |
| 测一个模型每秒大约能执行多少次 | [npu_bench：模型测速](device/npu_bench/README.md) |
| 看模型内部哪些计算步骤耗时、是否有步骤在 CPU 上运行 | [rknn_profile：内部耗时分析](device/rknn_profile/README.md) |
| 看设备频率和温度，或在实验时固定频率 | [npu_freq：频率查看与调整](device/npu_freq/README.md) |
| 给设备配置有线网络、Wi-Fi 或现场维护网络 | [first_net_config：网络配置](device/first_net_config/README.md) |
| 查看 GPIO 引脚、读取或设置电平 | [gpio_test：GPIO 控制](device/gpio_test/README.md) |

前两个 NPU 工具需要模型文件，但不需要摄像头或视频；它们已经有可执行程序时，可以直接运行，
没有程序或修改源码后再用各自的 `build.sh` 编译。定频工具是 Shell 脚本，直接运行。
三个工具各自的 README 都从进入目录开始，提供可以照着操作的步骤。

## 目录与构建入口

```text
tools/
├── check.py                   # 审核整改的代表性回归入口（不安装、不部署）
├── build/                     # Logic 清单与上报模板生成器
├── setup/                     # 平台依赖准备与板端环境检查
├── device/                    # 在设备上独立运行的配置与诊断工具
│   ├── first_net_config/      # 首次网络配置终端工具
│   ├── gpio_test/             # GPIO 控制与测试工具
│   ├── npu_bench/             # 模型独立推理延迟与吞吐
│   ├── rknn_profile/          # RKNN 逐层性能分析
│   └── npu_freq/              # CPU/NPU/DDR 频率状态、锁频与恢复
└── offline_dev_install/       # 按目标发行版隔离的离线开发环境与制包工具
    ├── debian/
    └── ubuntu/
```

应用构建的唯一公开入口是仓库根目录 `build.sh`。引擎 CMake 中间产物统一写入
`build/engine/`，完整应用包统一写入 `dist/<app>/`。

`device/` 中的 C/C++ 程序拥有各自的 `build.sh`，不会参与主引擎的默认构建；Shell 工具直接运行。`offline_dev_install/`
中的脚本应从仓库根目录调用，其输出分别保存在对应发行版目录的 `output/` 中。

NPU 诊断工具按功能分目录，C++ 工具的可执行文件直接生成在各自源码目录：

- [npu_bench](device/npu_bench/README.md)：`./tools/device/npu_bench/build.sh`，构建模型独立推理测速工具。
- [rknn_profile](device/rknn_profile/README.md)：`./tools/device/rknn_profile/build.sh`，查询逐层耗时。
- [npu_freq](device/npu_freq/README.md)：直接运行 `./tools/device/npu_freq/npu_freq.sh`，无需编译。

上述构建命令从仓库根目录执行，也可从其他工作目录通过绝对路径调用各自的 `build.sh`。
两个 C++ 工具使用仓库锁定的 RKNN SDK，分别生成 `tools/device/npu_bench/npu_bench`
和 `tools/device/rknn_profile/rknn_profile`；构建不会启动推理或改变设备频率。

在依赖已经安装的环境执行 `python3 tools/check.py`，依次检查后端关键回归、
前端构建/传输与终端算法回归、C++构建及非硬件CTest。它是代表性回归集，
不替代各模块的完整测试。用 `--suite browser` 单独运行终端连接、终端显示、
实时日志和配置同步浏览器回归；浏览器需要 Playwright 和 Chromium，
可通过 `CHROMIUM_PATH` 指定浏览器。`--suite` 支持选择多项，`--dry-run` 仅列出命令。

硬件推理测试带 `hardware` 标签，需准备模型并开启相应 CMake 选项后，
在构建目录手动执行 `ctest --output-on-failure -L hardware`。硬件测试会占用 NPU。
