# 引擎与独立业务项目重构验收

验证日期：2026-09-18 至 2026-09-19。设备：当前 RK3588/AArch64 开发板。
基线：提交 `e7f171b85aaa0da44eab4b67f3ae14cfc2531c47` 的干净源码归档。
重构产物来自当前工作树，发布记录中标记 dirty；工作区原有 GPIO 服务修改保留。

## 共享引擎阶段（当前实现，2026-09-19）

当前公共引擎已改为 `librkvision.so.1`；项目可执行程序保留自身业务、启动入口与嵌入清单。
本阶段性能基线为转换共享库之前的单体 `person_count`，已单独保存在 `/tmp/rkvision-shared-before/`。
下面保留的机器码逐字节核对只适用于此前源码 SDK 阶段；共享库新增 PIC 与动态链接布局，不沿用该结论。

- 16 个平铺项目全部通过板端 `build.sh package`，应用中只注册各自所属业务，全部复用同一份引擎库。
- 17 份配置检查：14 份通过；行车、继电器、GPIO 课程的 3 份初始配置按预期因无启用通道被拒绝。
- 仓库外项目通过直接 CMake 完整构建及打包；`--no-bundle-libs` 仍包含引擎库。
- 项目/SDK/共享库集成测试 12 项，安装替换测试 5 项通过；覆盖 ABI 不兼容拒绝、目录搬迁、
  不修改可执行文件替换兼容运行库、应用参数 schema 生效及中文系统依赖收集。
- 本地 RTSP 实流解码与 NPU 推理、断流重连、无效配置拒绝后继续运行、有效配置发布第 2 代快照、
  全局业务执行与 SIGINT 正常退出均通过。
- 应用 RUNPATH 为 `$ORIGIN/libs`；引擎 SONAME 为 `librkvision.so.1`、RUNPATH 为 `$ORIGIN`。
  发布库使用普通文件，压缩包不依赖符号链接；`app.json` 保存库 SHA256。
- 本轮实测为原生板端构建；Docker 不在当前环境中，交叉编译分支尚未实测。

记录：[项目与配置](validation/shared-runtime-projects.json)、[验收汇总](validation/shared-runtime-acceptance.json)。
使用方法见[独立项目与共享引擎](../development/independent-projects.md)。

### 共享库性能对照

同一 RK3588、同一三路 YOLOv8 配置、同一模型和视频，NPU 核心分配 0/1/2，关闭显示和 RTSP 输出。
每组按“旧版、共享库、共享库、旧版”交替，全部 12 次运行正常退出。吞吐为三通道平均 FPS 的总和。

| 条件 | 单次时长 / 排除预热 | 旧版总 FPS | 共享库总 FPS | 变化 |
|---|---|---:|---:|---:|
| 首轮默认调频、不绑定 CPU | 32s / 5s | 133.53 | 125.05 | −6.35% |
| CPU/NPU performance，绑定 CPU 4–7 | 62s / 5s | 172.00 | 171.85 | −0.09% |
| 恢复默认调频、不绑定 CPU，延长预热 | 62s / 20s | 127.76 | 132.46 | +3.67% |

首轮差异触发了受控复测，未作为通过结果忽略或删除。固定运行条件后，平均推理链耗时为
17.340 → 17.380 ms，CPU 使用量为 35.41% → 35.75%（100% 表示一个 CPU 核心），
RSS 为 104.71 → 104.63 MiB。默认调频的两组结果方向相反，说明这组场景对运行条件和调度波动敏感；
不能把默认条件的单轮差异直接归因于共享库，也不把后一组的增长声明为引擎优化收益。

本次验证没有发现可重复的明显运行性能回退；不承诺所有模型、显示/录像组合或长期负载绝对零差异。
临时 performance 调频已在测试后恢复到原 CPU ondemand / NPU rknpu_ondemand，未写入任何永久配置。

记录：[首轮](validation/shared-runtime-performance-initial.json)、
[固定运行条件](validation/shared-runtime-performance-controlled.json)、
[默认条件延长预热](validation/shared-runtime-performance-default.json)。

## 此前源码 SDK 阶段的结论与边界

业务已可在框架仓库外创建、编译和打包，业务源码仅依赖公开 SDK 头文件。
引擎和业务继续组合链接为一个原生可执行程序，原有优化参数、数据布局和逐帧调用方式保留。
65 个原有 C/C++ 源文件目标文件的可执行 ELF 段逐字节一致。
该核对覆盖目标文件的机器指令；最终 ELF 的链接布局有所变化，因此另做了实机运行对照。

最终独立项目的三路压力场景总吞吐从 129.71 FPS 变为 129.70 FPS，差异约 −0.01%；单帧推理耗时从
22.796 ms 变为 22.549 ms，差异 −1.08%。结合各轮波动和机器码核对，未发现明确的新增运行开销。
这些结果覆盖本次设备、模型与配置；不构成所有模型、温度、负载和长期运行场景绝对零差异的保证。

## 课程项目平铺（此前阶段，目录结构沿用）

课程已拆为 `projects/course_01` 至 `projects/course_10` 和 `projects/course_gpio`，与其他五个应用直接并列，共 16 个项目。
原 tutorials 聚合目录及其旧 build/dist 已删除。每个课程有独立的 build.sh、CMakeLists.txt、project.json、logic 和 assets。
课程逻辑及模型保持原内容，课程 01 默认保留无推理入门配置，同时保留可选推理配置。

- 11 个课程分别通过 CMake 配置、独立业务目标编译；每个目录只注册自己的一个课程逻辑。
- 11 个课程目标文件的可执行代码段均与原始基线逐字节一致。
- 课程 08 通过完整 Shell 编译与打包，包内只注册 logic_course_08；默认配置校验和所属上报模板检查通过。
- 检查各课程的配置逻辑 ID、模型路径及资源归属通过；项目/SDK/向导测试 8 项通过。
- 空 global_modules 目录添加 .gitkeep，保证 Git 克隆和源码交付后目录完整。

记录：[courses-layout-checks.json](validation/courses-layout-checks.json)。
下面的六项目记录保留为此前阶段的验收结果。

## 独立项目与 Shell/CMake 构建验收（此前阶段，2026-09-19）

该阶段结构为 `projects/` 下六个应用：吊机安全、人数统计、目标检测、课程示例、Dify 和继电器控制。
旧合并参考项目及 build.py 已删除。每个项目都具备 build.sh、CMakeLists.txt、project.json、logic 和 assets。

- 六个项目均通过自己的 `./build.sh package` 完成原生 Release 编译和打包，各自包含完整发布资源。
- 检查六个包内二进制的通道/全局注册列表：仅包含对应项目业务，总计保留 19 个通道模块、2 个全局模块。
- 外部工程 `/tmp/rkvision-cmake-external-vw5dyo8z/external_app` 通过标准 `cmake -S/-B`、`cmake --build` 编译并打包，未设置 VISION_PROJECT_DIR。
- 项目创建、SDK 头文件、引擎选择、模块隔离及向导测试共 8 项通过。
- 原 assets 的 13 个文件均在相应项目保留；按项目独立交付需要复制公共模型，没有向引擎目录复制业务。
- 再次逐份核对六个项目中的目标文件：65 个原有源文件的所有可执行代码段均与基线一致，差异 0、缺失 0。
- 吊机、继电器和 GPIO 课程配置默认未启用通道，因此校验按预期提示 no enabled channels；需要设置实际输入与硬件参数后启用。其他项目配置通过校验。
- 吊机原仓库没有完整运行配置和安全帽专用模型，新增配置用于现场设置，未宣称完成其硬件联动验收。
- 构建入口改为 Shell/CMake；离线制包脚本已改用项目的 build.sh package，本次未重新下载完整离线依赖包或安装系统服务。

目标文件对照：[projects-code-comparison.json](validation/projects-code-comparison.json)。
注册与配置检查：[projects-configuration-checks.json](validation/projects-configuration-checks.json)；14 份配置通过，3 份默认停用配置按预期拒绝启动。

### 最终项目压力对照

按旧、新、新、旧顺序各运行 32 秒，使用与第一阶段相同的三路加速视频、YOLOv8 模型与配置。
新版本为独立 person_count 项目产物。四轮均正常退出，退出码 0。

| 指标 | 原版 | 独立项目版 |
|---|---:|---:|
| 三路总 FPS | 129.71 | 129.70 |
| 单帧推理总耗时 ms | 22.796 | 22.549 |
| CPU % | 107.994 | 106.033 |
| RSS MiB | 105.008 | 104.865 |

吞吐变化 -0.01%，推理耗时变化 -1.08%。
未锁频；结果需结合轮次波动和目标文件代码段一致性解读，不能推广为所有负载绝对零差异。
原始数据：[projects-performance.json](validation/projects-performance.json)。
初次在沙箱内运行无法打开 NPU 和硬件解码设备，未取得有效样本，已排除；解除限制后完成上述测试。

## 第一阶段构建与功能验证（历史记录）

- 原版本与重构版本均使用板端原生 Release；保留 `-O3 -ffast-math` 和 Release 的 `-DNDEBUG`。
- 全量参考项目：19 个通道模块、2 个全局模块，注册列表与基线完全一致。
- 原 assets 的 13 个文件、模块 manifest/模板的 25 个 JSON 文件逐字节保留。
- `/tmp/rkvision-external-smoke` 位于框架仓库之外，创建、检查、编译、打包成功。
- 业务通过 `vision_app` OBJECT 目标编译，只接收公开 SDK、项目及 OpenCV 的 include 路径。
- 公开头文件逐个仅使用 SDK 和 OpenCV include 路径通过 C++14 语法编译。
- 增量构建验证：新增全局模块后注册及嵌入 schema 出现；删除后两者均自动移除。
- 参考项目完整打包，生成 4 个上报模板；包内程序能解析随包运行库并列出模块。
- 旧构建/安装入口、旧头文件转发层和引擎目录中的旧构建缓存已删除；离线制包统一调用 `vision package`。
- GPIO SDK 示例单独编译通过（仅编译，没有操作硬件输出）。
- Python 项目/SDK/向导测试：7 项通过。
- Web 后端：60 项通过，包括资产保留、明确替换、安装失败回滚。
- React 前端 `npm run build` 通过；保留原有的大 chunk 提示。
- 文档链接检查与 `git diff --check` 通过。

## 目录整合复验（2026-09-19）

- 删除 11 个旧头文件，SDK 成为公共接口的唯一定义；全局线程生命周期声明归入私有的 `global_logic_runtime.h`。
- 删除旧构建/安装脚本、兼容构建实现和独立开发向导启动器。统一使用 `vision build/package/install/develop`。
- 网络与硬件工具迁入 `tools/`，平台安装及 Debian/Ubuntu 离线工具迁入 `setup/`，原位置不留副本或链接。
- 删除引擎与已迁移硬件工具中的旧 CMake 缓存；应用产物保存在项目的 `build/`、`dist/`。
- 全量参考应用重新编译、打包通过；框架仓库外的应用重新编译通过。
- 再次对比基线：65 个原有目标文件的可执行代码段一致，差异 0，缺失 0。
- 再次通过 RTSP 解码、断线重连、无效配置拒绝、有效配置热更新、全局业务及正常退出验证。
- 7 项项目/SDK/向导测试、18 个 Shell 脚本语法检查、迁移后 GPIO/继电器工具编译、前端构建通过。
- 新部署路径下的只读运行环境检查通过；实际执行离线源码复制，确认新入口齐全、旧目录未进入源码包，复制后的参考应用校验通过。
- 完整离线依赖包未重新下载或安装；本次未执行系统部署。已有离线发布产物属于历史构建结果，需要重新制包才能包含此次调整。

## 第一阶段实机性能对照（历史记录）

相同本地 H.264 视频、YOLOv8 检测模型、ROI 人数业务、RKNN 2.4.2a2，固定每通道使用一个 NPU 核。
关闭 HDMI 和 RTSP 输出，启用解码、推理、后处理、跟踪和通道业务。
每场景按“旧、新、新、旧”顺序运行，每轮 32 秒，丢弃各通道第一条性能日志作为预热。
记录引擎现有的 5 秒性能窗口，以及 `/proc` 的 CPU 时间和 RSS。CPU 100% 表示占用一个 CPU 核。

| 场景 | 旧版每路 FPS | 新版每路 FPS | 旧版推理总耗时 ms | 新版推理总耗时 ms |
|---|---:|---:|---:|---:|
| 普通视频，1 路 | 25.120 | 24.750 | 26.274 | 26.111 |
| 普通视频，3 路 | 24.717 | 24.890 | 26.907 | 26.946 |
| 加速视频，3 路压力 | 43.097 | 42.983 | 22.749 | 22.763 |

普通视频受约 25 FPS 的源帧率限制；日志窗口中的瞬时 FPS 有采样波动，单路均值变化为 −1.47%。
压力输入通过缩短文件时间戳间隔产生，实测出现任务替换，确认输入速率超过推理处理速率。
压力测试旧版两轮分别为每路 42.407 / 43.787 FPS，新版为 44.720 / 41.247 FPS。
未锁定 CPU/NPU 频率，动态调频、视频内容与系统调度构成这组测试的实际波动范围。

| 场景 | 旧版 CPU % | 新版 CPU % | 旧版 RSS MiB | 新版 RSS MiB |
|---|---:|---:|---:|---:|
| 普通视频，1 路 | 33.809 | 33.290 | 80.791 | 81.117 |
| 普通视频，3 路 | 86.327 | 86.974 | 104.069 | 103.521 |
| 加速视频，3 路压力 | 108.502 | 109.481 | 104.624 | 105.255 |

原始统计：[refactor-metrics.json](validation/refactor-metrics.json)。
目标文件核对：[refactor-code-comparison.json](validation/refactor-code-comparison.json)。
所有性能测试进程均正常退出，退出码为 0。

## RTSP 与热更新验收

另用本机 GStreamer RTSP Server 生成 H.264 实时测试流，启用 YOLOv8 检测、通道人数业务及全局聚合：

1. RTSP 接收、MPP 解码、NPU 推理运行正常。
2. 主动关闭测试 RTSP 服务，观察到 OFFLINE；重启服务后 ONLINE，推理恢复。
3. 写入无效配置，加载被拒绝，原运行配置继续推理。
4. 恢复有效配置并修改 FPS，热更新完成，推理和全局业务继续运行。
5. SIGINT 后退出码为 0，全局线程正常退出。

初次测试受宿主 `http_proxy` 等代理环境影响，GStreamer 本机 RTSP 请求失败。
验收进程清除其继承的代理后通过；没有为此改动引擎运行代码或系统代理配置。

## 复测入口

```bash
# 对比原版与六个项目的目标文件；任一副本不一致或原模块缺失时退出非零
python3 tools/project/compare_code.py /path/to/baseline/build projects/*/build/<配置目录>

# config 应启用 performance_display，模型/视频用绝对路径或选择正确 cwd
# 两个程序使用同一份 RKNN 库；输出目录必须尚不存在
LD_LIBRARY_PATH=/path/to/framework/vision_analysis/vendor/rknn/2.4.2a2/lib/aarch64 \
python3 tools/project/benchmark.py \
  --before /path/to/baseline/vision_analysis \
  --after /path/to/new/vision_analysis \
  --config /path/to/test-config.json --cwd /path/to/project \
  --output /tmp/vision-comparison --seconds 32
```

复测性能时让设备保持空闲，不同时编译或运行其他视觉应用。
Docker/Windows 交叉编译路径本次没有可用环境进行执行验证；HDMI/RTSP 输出、USB 相机、现场摄像头
和长时间稳定性仍需对应部署环境验收。实机性能对照覆盖解码到业务的输入链路。
