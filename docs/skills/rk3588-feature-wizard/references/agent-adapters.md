# Codex 与 Claude Code 适配

本向导只正式支持 **Codex CLI** 和 **Claude Code**。两者共享同一套项目 Skill、需求访谈、Logic-only
白名单和验收合同；启动参数与权限模式只在适配器中区分。不得把“模型能生成文本”等同于“本地代理能
读写仓库并执行验证”。

## 选择规则

运行 `./develop_feature` 或 `develop_feature.cmd` 时：

1. 同时探测 `codex` 和 `claude` 是否在 `PATH` 中；
2. 在每条探测命令 15 秒超时内调用 `--version` 和 `--help`，确认当前版本仍包含适配器依赖的参数；
3. 只有一个可用时直接选择；两个都可用时显示编号菜单，由用户选择，默认保持 Codex；
4. 都不可用时停止，不回退到未经验证的第三方程序；
5. `--agent codex` 或 `--agent claude` 可跳过选择菜单。

`--provider` 是 `--agent` 的等价别名。`--profile` 和 `--no-alt-screen` 是 Codex 专用参数；未显式
选择代理时，使用这两个参数会自动选择 Codex。`--model` 原样交给所选代理，向导不猜测模型名称。

## 权限映射

| 向导模式 | Codex CLI | Claude Code | 工作流行为 |
|---|---|---|---|
| 默认 `auto` | 隔离副本 `workspace-write` + `never` | `dontAsk` + 两条 Logic `Edit(path)` 规则；条件满足时强制 Bash 沙箱 | 不弹权限确认；只回写 Logic 白名单 |
| `--confirm-before-code` | 同上 | 同上 | Skill 在编辑前只等待业务确认，不等待工具权限确认 |
| `--plan-only` | `read-only` + `never` | `dontAsk` + 只读工具 | 只读核对、合同和计划，隔离副本不回写 |

这里的“自动权限”是白名单边界内不询问，不是整机 Full Access。不得加入 Codex 的
`danger-full-access`/危险跳过参数或 Claude Code 的 `bypassPermissions`：它们会移除保护边界，与“不修改
其他文件”冲突。命令超出边界时直接失败，不让用户手动升级。代理自身的登录状态和组织策略仍可能进一步
收紧权限。

Codex 会话还用 CLI 覆盖清空额外 `writable_roots`，排除通用临时目录写权，关闭命令网络、Apps、插件、
Skill 的 MCP 依赖安装、非托管 hooks，并传入空 MCP 表。它们都不是生成本地 Logic 所需能力，不能让用户
profile 重新扩大本次隔离会话；Codex 自身的登录和会话状态文件仍由 Codex 在其配置目录正常维护。

Claude 在 macOS 直接启用强制 Bash 沙箱；Linux/WSL2 只有同时检测到 `bwrap` 和 `socat` 才启用
`sandbox.enabled=true`、`failIfUnavailable=true`、自动允许沙箱内 Bash，并关闭 unsandboxed escape
hatch。依赖不齐或位于原生 Windows 时，不自动开放写入型 Bash，只自动允许只读工具和两个模块根目录的
`Edit(path)`；Claude 的该规则同时约束 Edit 与 Write 文件工具。两端都在临时项目副本运行，最后由
[`write_guard.py`](../scripts/write_guard.py) 进行第二层整批拒绝/白名单回写。

Claude 的 `--tools` 还会把可见内置工具收敛到 Read/Glob/Grep、受 `Edit(path)` 约束的 Edit/Write，及
仅在强制沙箱可用时的 Bash；不向本次会话提供 Agent、AskUserQuestion、WebFetch 等无关工具。空
setting sources 和 strict MCP config 用于隔离用户/项目设置中的 hooks、附加目录与 MCP 能力。

Claude 不直接启动交互式 TUI。启动器使用官方 `-p` 非交互模式、JSON Schema 结构化结果和固定 UUID
保存会话；Claude 返回 `wait` 时，启动器在自己的终端提示中读取一条用户回答，再用 `--resume` 续接同一
会话，返回 `done` 后才进入校验与回写。这样既保留逐步澄清，也按 Claude Code 的官方行为跳过一次性隔离
目录的 workspace trust 对话框；启动器不修改用户的 `~/.claude.json`。会话记录和登录元数据仍可能由
Claude Code 写入它自己的配置目录，不属于项目仓库写回。

## 常用命令

Linux、macOS 或 WSL2：

```bash
./develop_feature --check
./develop_feature --agent codex "新增人员区域告警"
./develop_feature --agent claude "新增人员区域告警"
./develop_feature --agent claude --plan-only "评估跨通道聚合"
```

原生 Windows PowerShell 或 CMD：

```powershell
develop_feature.cmd --check
develop_feature.cmd --agent codex "新增人员区域告警"
develop_feature.cmd --agent claude "新增人员区域告警"
```

使用 `--dry-run` 只查看启动提示，不检查或启动代理。使用 `--check --agent claude` 可把“Claude Code
未安装或参数不兼容”反映到进程退出码；Codex 同理。

## 适配边界

- 启动器确认的是可执行文件、版本命令和必要 CLI 参数，不替用户完成登录，也不证明 API/订阅可用；
- 两个代理都收到同一份模型无关提示，并被要求从显式路径完整读取总控和专项 `SKILL.md`；
- Codex 使用自身交互式 TUI；Claude 的提问和回答由启动器通过 `-p`/`--resume` 逐轮转发；
- 两个代理都不以原仓库为工作目录；任何白名单外变更只可能存在于随后删除的临时副本，并会阻止整批回写；
- `agents/openai.yaml` 只是 Codex/OpenAI 产品元数据，不是项目知识真源，Claude Code 无需读取它；
- Claude Code 不需要把本仓库 Skill 复制到 `.claude/skills/`，因为启动提示会直接指定现有文件；
- 未支持 Gemini、Cursor、Qwen、DeepSeek 或任意 API Agent；不得在文档中声称已支持。

## 二次开发边界

| 文件/接口 | 唯一职责 |
|---|---|
| `scripts/agent_adapters.py` | `AGENT_DEFINITIONS`、可执行文件探测、无提示权限映射与安全 argv 拼装 |
| `scripts/start_wizard.py` | 系统环境检测、双代理选择、模型无关启动提示和会话生命周期 |
| `scripts/write_guard.py` | 当前项目隔离复制、文件哈希、路径分类、越界整批拒绝和 Logic 白名单文件回写 |
| 仓库根目录 `develop_feature` | 用当前 Python 定位并运行向导，不包含代理或框架知识 |
| `develop_feature.cmd` | 在原生 Windows 依次尝试 `py -3` 与 `python` |

维护适配器时，只把命令拼装和权限差异放在 `agent_adapters.py`；框架事实继续只维护在各 Skill 参考页。
新增或改变参数后必须模拟两种代理的命令、测试允许/越界文件变化，并在实际安装了对应 CLI 的环境运行
`--check`。不得通过 shell 字符串拼接启动代理；提示词必须作为单独 argv 传递，避免路径、引号或需求
文本被 shell 解释。

官方参数边界：

- [Codex CLI 命令参考](https://learn.chatgpt.com/docs/developer-commands.md?surface=cli)
- [Claude Code CLI 参考](https://code.claude.com/docs/en/cli-usage)
- [Claude Code 非交互模式与结构化输出](https://code.claude.com/docs/en/headless)
- [Claude Code 会话续接](https://code.claude.com/docs/en/sessions)
- [Claude Code 权限模式](https://code.claude.com/docs/en/permission-modes)
- [Claude Code 平台安装说明](https://code.claude.com/docs/en/installation)
