<div align="center">

# RK3588 多路视觉分析框架

**把视频、模型与业务规则，组织成可部署的边缘视觉应用。**

*Build configurable edge vision applications on RK3588.*

[![Platform: RK3588](https://img.shields.io/badge/Platform-RK3588-2563eb)](engine/vendor/rockchip/PLATFORM_COMPATIBILITY.env)
[![RKNN Runtime: 2.4.2a2](https://img.shields.io/badge/RKNN_Runtime-2.4.2a2-059669)](engine/vendor/rknn/2.4.2a2/RUNTIME_MANIFEST.txt)
[![License: GPL-3.0](https://img.shields.io/badge/License-GPL--3.0-blue.svg)](LICENSE)
[![Page views](https://hits.sh/github.com/LaoWei2001/rk3588_visual_analysis_framework.svg?label=Page%20views&color=7c3aed)](https://hits.sh/github.com/LaoWei2001/rk3588_visual_analysis_framework/)

[效果展示](#效果展示) · [快速开始](#快速开始) · [模型与业务接入](#模型与业务接入) · [性能实测](#性能与验证) · [文档中心](docs/README.md) · [项目愿景](#项目愿景)

</div>

面向 **Rockchip RK3588** 的 C++ 多路视觉框架，将视频接入、RKNN 推理、目标跟踪、业务规则、事件证据与 Web 运维连接在一起。你通过公开 SDK 编写业务，框架负责共用的视频管线、模型调度、媒体存储与应用管理。

适合需要多摄像头分析、区域告警、工业视觉或现场维护的边缘项目，也适合学习一套视觉应用从算法接入到板端交付的完整实现。

## 效果展示

**多路同时推理，每一路都能配置自己的业务逻辑。**

在浏览器中查看多路实时画面与分析结果。每路视频可以独立选择模型、配置业务逻辑，将目标检测、区域判断和告警规则组合成自己的应用。

[![Web 实时画面：多路视频同时推理，每路可独立配置模型与自定义业务逻辑](image1.png)](image1.png)

**在画布上，把视频、模型和逻辑连接起来。**

拖拽节点、连接分析流程，直观看到每一路视频使用的模型和逻辑。通过 C++ SDK 编写的自定义业务，也可以接入画布，在页面中配置参数。

[![Web 配置画布：为各路视频连接模型与逻辑函数，分别配置分析流程](image.png)](image.png)

*点击图片可查看原图。*

**[从快速开始部署到 RK3588，接入自己的视频与模型，查看实际效果 →](#快速开始)**

## 核心能力

| 能力 | 你可以做什么 |
| --- | --- |
| **多路视频，一套管线** | 接入 RTSP、文件与 USB 视频，复用相同视频源的解码，输出 HDMI 拼接画面和 RTSP 视频流。 |
| **多模型组合分析** | 组合检测、旋转框、姿态、分割与深度模型；按配置协调 NPU 核心，以有界队列控制积压，分别设置分析、预览与录像节拍。 |
| **用 C++ 表达业务规则** | 通过公开 SDK 查询目标、绘制 ROI、维护跨帧状态、触发事件和控制 GPIO；通道 Logic 与全局 Logic 分别处理单路和跨路业务。 |
| **在浏览器里配置与运维** | 用图形画布连接视频、模型、业务和上报节点，管理应用启停、参数、实时画面、日志、事件与终端。模块参数由 Schema 驱动。 |
| **保留证据，追踪投递** | 为事件保存图片与前后录像，通过本地事件存储和独立上传服务记录发送状态、执行重试；支持按规则采集数据集。 |
| **从开发走向设备交付** | 构建可部署应用包，使用在线安装或离线 bundle；按需配置开机自启、程序恢复、系统看门狗与网络恢复。 |

```mermaid
flowchart LR
    Source["RTSP / 文件 / USB"] --> Capture["视频采集与帧调度"]
    Capture --> Infer["RKNN 多模型推理"]
    Infer --> Logic["跟踪与业务 SDK"]
    Capture -->|无模型时| Logic
    Capture --> Preview["HDMI / RTSP 预览"]
    Logic -->|结果与绘图| Preview
    Logic --> Events["事件证据与异步投递"]
    Web["Web 控制台"] -.->|配置与控制| Capture
    Web -.->|参数与动作| Logic
```

分析、预览和事件录像各有自己的节拍；业务结果与事件图片按同帧处理，实时预览使用最新视频帧叠加最近结果。实现细节见[项目指南](docs/guides/project-guide.md#技术架构)。

## 快速开始

以下操作在 **RK3588 板端**进行。验证环境为 **Debian 11 / Linux 5.10.160 / RKNPU 0.9.0**，仓库固定 RKNN Runtime `2.4.2a2`。板端配套 RKNPU、RGA 和 MPP，驱动及视频栈版本见[平台基线](engine/vendor/rockchip/PLATFORM_COMPATIBILITY.env)。

### 1. 获取源码，安装平台(需要互联网连接)

```bash
git clone https://github.com/LaoWei2001/rk3588_visual_analysis_framework.git
cd rk3588_visual_analysis_framework
sudo ./install.sh online
./install.sh status
```

安装器准备构建依赖并部署 Web、运行管理及 GPIO 等服务。已有环境可先用 `bash install_deps.sh --check` 检查依赖。**首次安装默认将继电器 `GPIO6_A2` 置低并保存状态，执行前请核对接线。**

断网新设备使用完整离线 bundle；`install.sh offline` 适用于依赖已准备好的设备。详见[离线交付](docs/guides/project-guide.md#部署与离线交付)。

### 2. 构建第一个应用

<!-- 准备一段自己的、MPP 可解码的 H.264/265 MP4文件，将下面的 `/path/to/input.mp4` 替换为实际路径。`readme_demo` 请使用一个尚未安装的新应用名。 -->

```bash
./build.sh readme_demo
cp docs/examples/quickstart.json dist/readme_demo/assets/quickstart.json
cp /path/to/input.mp4 dist/readme_demo/assets/input.mp4
sudo ./install_app.sh readme_demo
```

`build.sh` 会把 `projects/assets/` 复制进应用包，交付他人前请筛选现场配置、凭据和媒体。上述示例视频只复制到构建后的包中。

### 3. 在 Web 中启动

打开 **`http://<设备IP>:8080`**，使用板端 Linux 系统账号登录：

1. 在程序管理中选择 `readme_demo` 和 `quickstart.json`，点击“启动”。
2. 打开“实时画面”，查看循环播放的视频；也可用 VLC 打开 `rtsp://<设备IP>:8554/live`。
3. 点击红色“停止”，等待页面确认退出结果。

[最小配置](docs/examples/quickstart.json)以文件循环播放和 RTSP 输出跑通视频链路。接下来将 RK3588 RKNN 权重及所需标签放入已安装应用的 `assets/`，在配置编辑器中添加模型、开启推理并选择业务模块，即可逐步搭建自己的视觉应用。具体操作见 [Web 控制台手册](docs/skills/rk3588-console-ops/references/web-console-user-guide.md)。

Web 停止流程会等待引擎完成清理，核对退出记录并展示对应结果。生命周期与日志说明见[程序停止说明](docs/operations/application-recovery.md#用户主动停止与退出结果)。

<details>
<summary><strong>命令行运行与配置检查</strong></summary>

先停止其他视觉实例，在仓库根目录执行：

```bash
cd dist/readme_demo
export ASSETS_DIR="$PWD/assets"
export LD_LIBRARY_PATH="$PWD/libs:$LD_LIBRARY_PATH"
./vision_analysis --validate-config ./assets/quickstart.json
./vision_analysis ./assets/quickstart.json
```

日志输出到当前终端，按 `Ctrl+C` 停止。`--validate-config` 在启动前完成配置结构与资源路径检查。无界面运行使用上述直接启动方式；HDMI 运行可使用包内 `run.sh`，该脚本会把所选配置的 `enable_display` 改为 `1`。

平台以一个视觉应用统一托管多路视频，各通道在同一应用内独立配置模型与业务。

</details>

## 模型与业务接入

### 选择模型，组合任务

| 任务 | `model_type` | 主要结果 |
| --- | --- | --- |
| 目标检测 | `yolov5`、`yolov8_det` | 类别、置信度、目标框 |
| 旋转目标检测 | `yolov8_obb` | 旋转框四角与包围框 |
| 姿态估计 | `yolov8_pose`、`yolo26_pose` | 目标框、关键点与置信度 |
| 分割 | `yolov5_seg` | 检测结果与整帧类别掩码 |
| 米制深度 | `yolo26_depth` | 米制深度图 |
| 相对深度 | `zip_depth` | 相对逆深度图，数值越大越近 |

同一通道可以组合多个模型，**每个模型独立读取输入尺寸，结果统一映射到 `640×640` 业务坐标**。业务通过 `model_id` / `model_type` 选择结果，查询目标、分割或深度数据。详见[模型结果契约](docs/reference/model-result-contract.md)与[坐标约定](docs/reference/business-coordinates.md)。

将面向 RK3588 导出的 RKNN 权重放入应用 `assets/`，按适配器契约配置输入输出；`yolov5`、`yolov5_seg`、`yolov8_det` 配套对应标签文件。各模型的结果语义与板端接入记录见[模型结果契约](docs/reference/model-result-contract.md)。

### 扩展更多模型

**框架已经接入 ZipDepth 深度估计作为示例。** ZipDepth 输出相对逆深度，表达画面中的远近层次；YOLO26 Depth 提供米制深度。深度模型与检测、姿态、分割共享视频管线、NPU 调度和业务结果接口，可以按场景组合使用。用这两个模型来说明本项目架构适配其他模型的灵活性。

新增模型时，通过 [`ModelBase`](engine/src/vision/models/model_base.h) 实现前后处理与结果契约，在模型工厂中注册，并同步配置校验、能力目录和 Web 模型识别规则；重新构建后，新模型即可接入现有分析管线。具体步骤见[新增模型类型](docs/guides/project-guide.md#新增模型类型)。

### 用一个模块实现业务

业务放在 `projects/modules/`，通过 `logic.cpp` 编写规则、`logic.json` 声明参数与输出。例如，查询整帧中的人员并发布数量：

```cpp
#include <channel.h>

static void logic_person_count(ChannelContext *ctx)
{
    if (!ctx) return;
    TargetQuery query;
    query.labels = {"person"};
    query.min_score = 0.5f;
    query.roi = ROI_FRAME;
    const auto people = ctx->query_targets(query);
    if (people.valid())
        ctx->publish_int("person_count", people.count);
}

REGISTER_LOGIC(logic_person_count);
```

在同目录的 `logic.json` 中声明 `person_count` 为 `integer` 输出，重新构建后即可在通道中选择 `logic_person_count`。将 `person` 设置为模型实际标签，每次有效查询都会发布最新人员数量。模块清单、参数表单和注册流程见[业务工作区](projects/README.md)。

进一步实现“区域中持续有人 → 绘制告警 → 保存证据 → 投递事件”，可以直接阅读[区域持续占用报警示例](projects/modules/logic_roi_dwell_demo/README.md)。跨通道联动放在 `projects/global_modules/`，通过全局 Logic 消费各通道发布的数据。

**[SDK 功能索引](engine/include/README.md) · [目标查询](docs/sdk/target-query.md) · [现有业务示例](docs/skills/rk3588-channel-logic/references/current-examples.md)**

SDK 提供随应用编译的源码接口；更新业务模块后重新构建即可生成新的应用包。运行中的参数调整与状态策略见[热重载说明](docs/guides/project-guide.md#配置热重载)。

## 架构与目录

**引擎提供共用能力，业务模块定义场景规则，Web 与后台服务负责管理和交付。**

```text
.
├── engine/              C++ 视觉引擎
│   ├── include/         面向业务的公开 SDK
│   ├── src/             应用、管线、媒体、推理、业务框架与事件实现
│   └── vendor/          第三方依赖与平台版本清单
├── projects/            通道/全局业务模块、模型与运行资产
├── web_console/         React + TypeScript 前端、FastAPI 后端
├── services/framework/  运行管理、事件投递、模型更新与设备恢复
├── tools/               构建辅助、板端诊断与离线制包
└── docs/                使用指南、接口、运维与性能记录
```

三个根目录脚本分别负责：`build.sh` 构建应用包，`install.sh` 安装平台，`install_app.sh` 安装应用。代码职责与依赖边界见[整体架构](docs/architecture/overview.md)，C++ 内部目录见[引擎源码导航](engine/README.md)。

## 性能与验证

**完整视频管线下，YOLOv8 检测六路总吞吐达到 150.00 FPS，九路达到 186.45 FPS。**

2026-11-13，在 RK3588 / 8 GB 上对四种模型分别进行 **1、3、6、9、12、15 路同模型并发实测**。每路独立解码 1280×720、25 FPS 的 H.264 文件，模型输入均为 640×640；同时开启 **SORT、本地拼接显示和 H.264 硬件 RTSP 输出**，由本机 FFmpeg 持续解码播放。

每通道 3 个 worker，NPU 自动分配，`queue_size=1`、`max_fps=30`，拼接画布 1280×720，通过 DSI 屏显示。每组预热 10 秒、采样 30 秒，15 路深度采样 60 秒，按正反顺序完成两轮。

**单元格：平均每路成功推理 FPS（总 FPS）。**

| 并发路数 | YOLOv8 检测 | YOLOv8 Pose | YOLOv5 Seg | YOLO26 Depth |
| --- | ---: | ---: | ---: | ---: |
| 1 路 | 25.00（25.00） | 25.00（25.00） | 25.00（25.00） | 20.81（20.81） |
| 3 路 | 25.00（75.00） | 25.00（75.00） | 25.00（75.00） | 6.56（19.69） |
| 6 路 | 25.00（150.00） | 22.32（133.90） | 16.10（96.61） | 3.36（20.16） |
| 9 路 | 20.72（186.45） | 14.60（131.37） | 10.62（95.58） | 2.27（20.46） |
| 12 路 | 15.22（182.59） | 10.63（127.55） | 7.85（94.22） | 1.72（20.64） |
| 15 路 | 11.94（179.06） | 8.28（124.25） | 6.23（93.38） | 1.38（20.77） |

本轮完成 **48 组正式测量**，各组全部通道持续推理与发布，RTSP 播放持续推进，**48 组均完成正常退出清理，采样窗口推理失败数为 0**。统计由两轮完整窗口的计数与实际秒数计算；结果发布 FPS、测试配置和统计方法见完整报告。

[完整测试报告](docs/performance/model-channel-fps.md) · [更多性能记录](docs/performance/README.md)

## 文档导航

| 我想…… | 从这里开始 |
| --- | --- |
| 编译、部署、离线交付 | [项目指南](docs/guides/project-guide.md) |
| 配置画布、看画面、管理应用 | [Web 控制台手册](docs/skills/rk3588-console-ops/references/web-console-user-guide.md) |
| 编写业务、查询接口 | [公共 SDK](docs/sdk/README.md)、[业务工作区](projects/README.md) |
| 采集训练数据、保存录像到电脑 | [数据集采集](docs/guides/dataset-collection.md)、[媒体保存](docs/guides/local-media-saving.md) |
| 排查停止、恢复、日志与设备问题 | [运行与维护](docs/operations/README.md) |
| 理解代码分工或查找其他主题 | [整体架构](docs/architecture/overview.md)、[全部文档](docs/README.md) |

## 贡献与许可证

欢迎一起完善模型适配、业务示例、板卡兼容性、可复现的性能记录与使用文档。发现问题时，请通过 [Issues](https://github.com/LaoWei2001/rk3588_visual_analysis_framework/issues) 提供板卡环境、脱敏配置、复现步骤和日志；提交代码前请阅读[贡献指南](CONTRIBUTING.md)。

项目基于 **[GNU GPL v3.0](LICENSE)** 发布，第三方库、字体和模型遵循各自授权。

作者：**Sunny_Wei** · [1927096839@qq.com](mailto:1927096839@qq.com)

---

## 项目愿景

本项目受到 GNU 计划与自由软件运动的启发。我们相信，软件不应只是一个无法理解和修改的封闭工具，
也应当成为可以学习、验证、改进和继续传播的公共知识。开源的价值不仅是公开代码，更是让工程经验
能够被后来者继承，让不同开发者在清晰的规则和可复现的实现上继续创造。

我们希望这套框架能够为 RK3588 及其他边缘计算平台上的多路视觉应用提供一种可借鉴的实现思路：
底层能力集中建设，业务模块独立演进，配置和运行状态可视化，现场问题能够定位，
常见故障(网络波动，系统崩溃等)可自动修复，整套系统能长时间稳定运行。
如果它能让一位新人更容易理解系统、让一个算法更快落地、让一个团队少重复搭建一次基础设施或者
部署时让使用者少在命令行输入几行命令，这个项目也就实现了它的意义。

本项目融合了作者对边缘视觉的开发，部署，维护的全部理解。如果本项目对你的学习、研究或工程实践有
所帮助，欢迎 Star，提出建议或参与改进，让更多开发者能够发现、使用并共同完善它。

祝朋友们编程愉快！Happy coding！:)
