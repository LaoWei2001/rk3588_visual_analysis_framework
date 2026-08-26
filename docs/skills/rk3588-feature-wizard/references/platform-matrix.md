# 开发宿主、部署目标与平台处理

## 三个环境必须分开

- **开发宿主**：当前运行 `develop_feature` 及所选 Codex/Claude 编程代理的系统，由启动器自动检测；
- **部署目标**：最终安装视觉程序的系统，通常但不强制是 RK3588 Linux；
- **验收环境**：能够接触模型、视频源、NPU、GPIO、网络和远端服务的测试位置。

自动检测只能证明当前主机的 OS、架构、设备树和命令是否存在，不能证明硬件、驱动、模型、视频源或
远端服务可用。用户确认的信息优先于误判，但要把修正记录为用户提供的环境事实。

## 第一轮环境问题

先展示启动器报告，把 RK3588 Linux 标成默认部署建议，然后只问：

> “检测结果和默认部署目标是否正确？回复‘是’即可；不正确可简短回复，例如：‘否，当前是 WSL2，
> 最终仍部署 RK3588。’”

不得要求用户列举 Python、工具、板卡、视频、NPU、GPIO 或远端服务。用户答“是”只确认开发宿主和
默认部署目标，不代表任何硬件已经验证；代理按功能只读探测，无法证明的项目自动进入“待实机验证”。
用户只答“否”时，最多追加一个短选项题纠正环境。

## 平台矩阵

| 开发宿主 | 启动入口 | 当前主机适合执行 | 必须按条件转移或保留 |
|---|---|---|---|
| RK3588 Linux | `./develop_feature` | 通用检查；依赖和设备齐备时可板端构建/运行 | 未接入的模型、视频、GPIO、远端服务仍不可声称验证 |
| Linux x86_64 | `./develop_feature` | 文档、清单、Python、Web；已有镜像时交叉编译 | RKNN/RGA/GPIO 和视频硬件放到板端 |
| Windows WSL2 | `./develop_feature` | Linux 命令、源码、Python、Web 和非硬件检查 | 设备能力取决于透传；通常保留板端验收 |
| 原生 Windows | `develop_feature.cmd` 或 `py .\develop_feature` | 源码、文档及已安装依赖支持的 Python/Web 检查 | 不直接运行 `build.sh`、systemd 或 RKNN/RGA 板端验证 |
| macOS | `./develop_feature` | 源码、文档及已安装依赖支持的 Python/Web 检查 | Linux 服务、交叉编译和硬件验证另选环境 |
| 未识别系统 | `--plan-only` 优先 | 只读检查和需求合同 | 确认 shell、工具链和沙箱后才允许自动实现 |

所有平台都先复制当前仓库的已跟踪/未忽略文件到系统临时目录，代理结束后只用 Git 补丁回写两个 Logic
模块根目录。Codex 在临时目录使用 workspace 沙箱和 `never` 审批，因此不需要手动 `/permissions`。
Claude 由启动器通过非交互 `-p` 和 `--resume` 转发多轮问答，不出现临时目录的 workspace trust
对话框。其强制 Bash 沙箱适用于 macOS，以及同时装有 `bwrap`、`socat` 的 Linux/WSL2。依赖不齐或位于
原生 Windows 时不自动开放写入型 Bash，只开放 Logic 路径的 `Edit(path)`（同时覆盖 Edit/Write 文件
工具）；静态 manifest 校验由启动器自己的 Python 执行。

WSL 优先使用 WSL2，并把仓库放在 Linux 文件系统而不是 `/mnt/c/`。Codex 当前不支持 WSL1；Claude Code
虽列出 WSL1，但不支持沙箱且官方记录了原生二进制兼容问题，因此本向导不把 WSL1 当作可靠自动开发环境。
Claude Code 原生 Windows 可以运行，但其沙箱只在 WSL2 支持；这不改变 RK3588 板端验证必须转移的事实。

代理选择与权限映射见 [`agent-adapters.md`](agent-adapters.md)。官方平台边界见
[Codex 沙箱](https://learn.chatgpt.com/docs/sandboxing)、
[Codex Windows/WSL2](https://learn.chatgpt.com/docs/windows/wsl)、
[Claude Code 安装与平台支持](https://code.claude.com/docs/en/installation)和
[Claude Code 沙箱](https://code.claude.com/docs/en/sandboxing)。

## 执行规则

1. 根据开发宿主选择路径格式、Python 命令和 shell，不把 Bash 命令原样交给原生 Windows。
2. 缺少 `rg` 时改用当前系统可用的只读搜索工具；缺少可选工具只缩小验证范围，不伪造结果。
3. 只有检测并实际调用成功，才能声称 Docker、交叉编译器、GStreamer、systemd 或硬件工具可用。
4. x86_64/WSL2/macOS/Windows 的测试通过不能替代 RK3588 NPU、RGA、GPIO、真实视频和性能验证。
5. 最终交付按“开发宿主已验证、目标环境待验证、外部系统待验证”分组报告。
