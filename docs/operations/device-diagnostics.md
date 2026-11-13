# 设备信息、性能设置与故障取证

## 不同 RK3588 板卡的故障取证

共用 RK3588 芯片不表示核心板或载板相同。不同厂商的内存、PMIC、电源、存储、网卡、
设备树和 BSP 内核可以不同；本功能按实际接口检测，不依赖某厂商的板卡名称、固定温区编号
或固定看门狗编号。“系统设置 → 设备基本信息”集中显示设备树型号、compatible、内核、
最近采样时间及取证接口能力和限制；故障恢复记录中仅保留每次事件自身的判断依据。

设备概况接口 `GET /api/system/device-info` 保留以下排查数据供兼容读取，仍要求登录。
“系统设置 → 性能”（`?section=performance`）独立展示实时曲线、利用率、频率、温度与性能/温控设置。
设备基本信息页仅展示板卡身份、系统版本、开机运行时间与折叠的取证能力；
网络、存储、服务、程序、时间设置继续由各自页面负责，不在设备页重复展示。
界面省去重复资源卡片、采样说明和利用率来源路径，保留读数、单位、保护上限、异常及保存结果。

接口数据范围：

| 分组 | 接口内容 |
| --- | --- |
| 系统概况 | 主机名、板卡/芯片型号、序列号、发行版、内核、架构/核心/最高频率、开机与运行时间、设备时间/时区/同步状态、当前程序与应用目录 |
| 资源与温度 | CPU 采样平均使用率、内存已用/可用、交换空间、系统负载、芯片/CPU/GPU/NPU 温度、已注册的 GPU/NPU 驱动及当前频率 |
| 存储空间 | 去重后的本地磁盘挂载位置、用途、文件系统、容量、已用/可用空间及只读状态；应用路径按实际符号链接位置匹配数据盘 |
| 网络概况 | 主要网卡及活动接口、链路、速率、IP、MAC、默认网关、DNS；省略回环、容器与无用的闲置虚拟接口 |
| 关键服务 | 控制台、运行协调、恢复/记录服务、网络管理、系统日志、升级与上传服务的当前状态和开机启动状态 |
| 取证能力 | 设备标识、采样时间、看门狗/复位原因/崩溃残留/压力接口与限制，放在折叠区域 |

停留性能页且页面可见时，每 1 秒采样 CPU / GPU / NPU 利用率、温度、频率、
内存；离开或隐藏页面后停止每秒轮询，不并发堆积请求。设备基本信息按 15 秒更新。网络地址、服务与时间同步
等慢查询只在内存缓存 15 秒。曲线仅在浏览器内存保留最新 60 个点，不保存历史文件。
NPU 三个核心在同一图中分别以蓝色实线、橙色虚线、紫色点线显示，附各核心当前读数与图例；
数值重合时仍可按线型辨认，单核心缺失读数会断开该段曲线，避免填零或沿用旧值。
未知指标显示“未提供”，首次 CPU 计数需要两个采样才有利用率。

“性能与温控设置”根据 CPUFreq / devfreq 实际频率表开放 CPU 各调频组、GPU、NPU
最大频率；温度阈值对应当前设备树与内核公开的 thermal trip。不提供电压、
强制电源、复位或关闭温控开关。临界阈值不能超过设备树原始值，不允许跨越阈值顺序、
回差间隔或低于当前温度的保护余量；不支持的项目明确标为只读。
CPUFreq 使用 kHz，devfreq 使用 Hz，温控使用毫摄氏度，API 与界面统一为 MHz 和 ℃。
CPU 最低频率可能包含内核 PM QoS 临时提频，不作为保存版本的依据，也不写入持久最低频率。
CPU 只提交上限，由内核协调范围；GPU / NPU 有需要时先降低最低频率。
写入后最多等待 0.5 秒让内核异步调频完成，再校验读回值，失败逆序回退。
回退也失败会明确报告。保存需管理员权限，版本冲突返回 409；轮询不会覆盖编辑草稿。

性能设置仅保存一个 `/etc/rk3588-runtime/performance.json`（最大 64 KiB，原子覆盖），
不另存配置版本或遥测日志。`rk3588-performance.service` 在开机时重新应用保存值；
未保存时不更改硬件。板卡型号、compatible、序列号、内核和接口档位指纹不匹配时跳过
旧设置并提示。恢复初始设置会恢复首次保存前的频率上限、GPU / NPU 最低频率及设备树原始温控阈值。
调参成功、失败和开机重应用归入“运行与操作记录”，受最新 20 条限制，不混入系统故障恢复。

NPU 优先读取 RKNPU 专用 `load`，显示各核心和平均利用率；专用接口缺失时不采用
已知可能持续显示 100% 的通用负载。读取 `load/version` 不主动挂载 debugfs，不读取
可能给设备上电的调试 `freq` 文件。依据：
[Rockchip RKNPU 驱动](https://github.com/rockchip-linux/kernel/blob/develop-5.10/drivers/rknpu/rknpu_debugger.c)、
[Linux CPUFreq](https://docs.kernel.org/admin-guide/pm/cpufreq.html)、
[Rockchip CPUFreq QoS 实现](https://github.com/rockchip-linux/kernel/blob/develop-5.10/drivers/cpufreq/cpufreq.c)、
[Linux thermal sysfs](https://cdn.kernel.org/doc/html/latest/driver-api/thermal/sysfs-api.html)。
临界阈值由内核保护策略使用，达到时可能触发关机或重启，并不是单纯的界面告警值。
本地采集读取 proc/sysfs 和固定的状态查询命令，命令输出有大小、超时和并发数量限制。
单项接口缺失时其他分组仍展示；首个 CPU 间隔样本尚未形成时显示“采样中”，缺失指标不填 0。
内存优先使用 MemAvailable，旧内核回退计算时标注“估算”；温度使用设备自己的临界值。
“设备已注册”“服务运行中”分别表示内核注册或服务状态，不能代替业务健康检查。
NTP 状态、频率或温度不被 BSP 提供时明确标为未提供；网络概况展示本机状态，不发起连通探测。

指标来源：[Linux proc 文件系统](https://www.kernel.org/doc/html/latest/filesystems/proc.html)、
[CPUFreq 属性](https://docs.kernel.org/admin-guide/pm/cpufreq.html)。

| 覆盖范围 | 依据 | 判断边界 |
| --- | --- | --- |
| 内核 panic、Oops、内核地址异常 | 当前开机归档的 pstore / 上次运行周期 journal | 确认异常发生；panic 不单独证明最终软件或硬件复位方式 |
| OOM、CPU soft/hard lockup、RCU stall、长时间阻塞 | 内核对应错误原文 | 与重启时间接近时列为相关异常，不能自动证明根因 |
| RK NPU、Mali / Panthor / Panfrost GPU、MPP / RKVDEC / RKVENC、IOMMU | 驱动超时、故障、地址错误日志 | 正常初始化不作为故障；驱动日志格式变体可能需要增补规则 |
| eMMC / SD / NVMe、文件系统、空间不足 | I/O 错误、超时、EXT4 / F2FS、只读状态及 statvfs | 日志错误和采样状态分别记录，不把空间低直接视为存储损坏 |
| 温度、供电 | 板卡 thermal critical trip 和内核危险温度/低电压告警 | 无传感器或告警时不能追溯证明瞬间掉电；不用统一温度常量 |
| 网卡 / USB 驱动 | 超时、控制器无响应、设备错误日志 | 普通断开不能仅凭日志判定硬件损坏 |
| 服务退出 | systemd SERVICE_RESULT / EXIT_CODE / EXIT_STATUS | 区分看门狗、超时、OOM、SEGV、ABRT、BUS、ILL、KILL；KILL 不能单独等同 OOM |
| 网络恢复 | 链路、IP、路由、网关、检测目标及 NetworkManager 状态 | 网关未响应 ICMP 可能为策略限制；DNS、上游、目标内部问题保留判断边界 |

`/var/lib/rk3588-runtime/recovery-diagnostics.json` 保存最新资源采样与异常。资源包括内存/交换区、
负载、可用 PSI、文件系统空间/只读状态、温度和已观察项目服务的 D 状态线程。
PSI 不存在时继续采集其他指标。采样周期内发生且未留下持久日志的瞬时故障仍可能无法追溯。

内核异常按启动周期关联，并过滤距日志末尾超过 180 秒的已知时间戳；上次 journal 使用最后一条
系统日志作为时间锚点，避免很早发生的驱动错误被归因给后来正常的重启。人工/定时等明确重启
请求优先于普通同期告警。资源低水位只能说明压力，不能据此确认 OOM；无正常关机标记且存在
上次采样时，记录非正常中断迹象及候选原因，不擅自断定掉电。

复位原因从已知只读 sysfs、设备树 chosen 和启动参数读取。厂商原始数字码不跨 BSP 套用含义；
未暴露接口时明确显示限制。采集不访问裸寄存器、不打开看门狗设备、不改变喂狗、设备树或内核。
新厂商专用复位接口仍需该 BSP 的文档和适配，不能仅靠 RK3588 型号保证取得所有硬件原因。
新增能力从升级后开始采样，不补造过去未记录的资源数据。

参考：[Radxa ROCK 5B 硬件资料](https://wiki.radxa.com/Rock5/hardware/5b)、
[Linux PSI 接口](https://www.kernel.org/doc/html/v6.10/accounting/psi.html)。

日志与诊断文件的大小和保留期限见[保留策略](log-retention.md)，系统恢复开关见[无人值守恢复](unattended-recovery.md)。

源码入口：`services/framework/runtime_manager/python/rk3588_runtime/device_info.py`、
`performance.py`、`fault_diagnostics.py`；Web 设置页在 `web_console/frontend/src/pages/settings/`。
本文描述接口和实现边界，不代表每种 RK3588 板卡都已做硬件故障或性能调参验收。
