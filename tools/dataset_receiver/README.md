# Windows 电脑采集接收器

`build_windows.py` 将现有 `dataset_receiver.py`、官方 Windows 嵌入式 Python 和 Go 启动器
构建为一个 Windows 10/11 x64 EXE。用户电脑无需安装 Python、Go、Tkinter 或额外包。
使用及部署说明见 [电脑采集文档](../../docs/computer-dataset-collection.md)。

```bash
python3 tools/dataset_receiver/build_windows.py
```

构建需要 Python 3 和 Go 1.23+，支持 Linux ARM64 上交叉构建。没有外部 Go 模块依赖。
脚本下载 Python 3.13.16 官方嵌入式运行时并校验固定 SHA-256；`--runtime-zip` 支持离线构建。
产物在 `web_console/backend/resources/receiver/windows-amd64/`，需要随控制台一起部署。
启动器和接收器变更后必须重新构建；构建时执行启动器测试。

构建产物就是通用 EXE，可直接复制给电脑运行；Web 下载返回相同文件，不附加设备信息或凭据。
首次填写设备地址、选择保存目录，后续记忆在 EXE 旁同名 JSON 中。采集接口免登录，
任务配置等控制台功能仍需登录。`--url` 指定设备地址，`--output` 指定保存目录。
每次打开 EXE 显示当前设置：回车开始接收，`1` 修改设备地址，`2` 更换保存目录，`3` 同时修改，`0` 退出。
菜单修改设备时可沿用当前目录，设备图片按地址分别存放。取消目录选择保留原配置。`--start` 跳过菜单；显式 `--url`、`--output` 或 `--choose-directory` 也直接按参数启动。
地址可直接填写 `192.168.2.41:8080`，自动补上 `http://`；HTTPS 填写完整地址。同一地址省略或补上协议不会重置保存目录。
启动器保留旧版 EXE 尾部配置读取能力，但仅使用地址，不再使用或保存旧凭据。
旧尾部地址只用于首次填充配置，后续用户保存的地址优先，防止重新打开时被覆盖。
内置运行时每次释放到新的私有临时目录，退出时清理。Python 子进程通过绝对路径启动，使用
`-I -u`，使用同一套 SQLite 和先持久化后确认的接收逻辑。

Web 当前存图由脚本内独立 `InventoryScanner` 统计所选目录中本设备/程序/通道实际存在的 JPG/JPEG。后台流式遍历不读取 SQLite，每 256 个条目短暂让出 CPU，每轮等待 5 秒；不会阻塞接收循环，目录 I/O 阻塞也不会延长退出。完整快照随现有轮询上报，删图、删文件夹和重命名后自动更新；累计保存和数量上限不变。此可选字段兼容旧后端，旧 EXE 继续接收，但新 Web 无法从旧 EXE 获取当前存图统计。

Windows 文件夹选择器使用系统 Shell API；控制台显示接收日志。
Ctrl+C 由启动器转发为接收子进程的 CTRL_BREAK，Python 将其作为 KeyboardInterrupt 处理，主动发送离线通知，再关闭 SQLite 和文件锁。离线通知整个等待最多 1 秒，失败时设备仍按原心跳超时判定。
子进程单独建立控制台进程组，避免信号递归；网络阻塞时等待 3 秒后终止，第二次 Ctrl+C 立即终止。
正常停止直接退出窗口，仅双击启动失败时等待回车显示错误。
启动器及子进程绑定 Windows Job Object，启动器关闭窗口、被强制终止或崩溃时均由系统终止所属子进程。
关闭窗口时，双方分别注册 CTRL_CLOSE_EVENT 处理器：接收器尝试离线通知，启动器最多等待 4 秒让子进程退出，避免 Job Object 提前终止通知。强制终止和崩溃仍由系统清理，不依赖退出回调。
使用 Windows 自带 Kernel32 接口，不增加系统依赖。经典控制台关闭 Quick Edit 并启用 Ctrl+C 处理，退出时恢复原设置。
同一设备和保存目录的本地接收编号持久化，重新打开无需等待旧进程的 30 秒在线租约；本地文件锁仍阻止同目录重复运行。
正常退出通知成功还会释放设备占用，更换目录或电脑也无需等待；异常退出或旧版后端仍保留 30 秒超时。

实现依据：[Microsoft 控制台信号说明](https://learn.microsoft.com/en-us/windows/console/generateconsolectrlevent)、
[Job Objects](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects)、[关闭窗口回调](https://learn.microsoft.com/en-us/windows/console/handlerroutine)、
[Python signal](https://docs.python.org/3.13/library/signal.html)。
构建时运行退出监督测试。`process_windows_test.go` 另外覆盖真实窗口信号转发和父进程强制退出后子进程终止，
这些 Windows 专用测试需在 Windows 构建机运行；Linux 上仅交叉编译验证。

在 Windows 上以不包含任何 Python 的 PATH 运行以下命令可检查内置组件：

```powershell
.\dataset_receiver.exe --check-runtime
```

`--licenses` 显示内置 Python 和 Go 组件许可证。
