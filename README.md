# RK3588 可扩展视觉分析运行框架与管理平台

面向 RK3588 边缘设备的多路视频分析平台，提供视频采集、RKNN 推理、目标跟踪、业务逻辑扩展、可视化配置、事件录像、可靠上报、模型 OTA 和运行运维能力。

项目由三层组成：

- **视觉分析运行框架**：C++ 实现的多路视频采集、推理、跟踪、业务逻辑和输出管线；
- **管理平台**：React + FastAPI 实现的可视化配置、程序管理、实时画面、日志、记录、终端和服务控制；
- **业务应用**：通过 `logic_xxx` 模块和 JSON 配置构建的具体分析方案，仓库默认提供 SOP 路径合规检测应用。

> 本项目不是通用的任意 DAG 工作流引擎。Web 画布用于编排视频源、模型、ROI、业务逻辑和上报等固定角色，运行时将其转换为稳定的多通道分析管线。

## 核心能力

### 视频与推理

- RTSP、视频文件和 USB 摄像头输入；
- 最多 15 个稳定通道 ID；
- YOLOv5 检测、YOLOv8 检测、YOLOv8 Pose、YOLOv5 Seg；
- 单通道多模型组合推理，结果按同一帧合并；
- RKNN NPU 多核心分配、RGA 图像转换和 DMA-BUF 零拷贝优先路径；
- 每通道独立 FPS、队列深度、阈值、类别过滤和跟踪参数；
- SORT 风格目标跟踪及稳定 `track_id`；
- 推理通道和无模型传统 CV 通道可同时运行。

### 可扩展业务逻辑

- 每种业务逻辑位于独立的 `src/logic/modules/logic_xxx/` 目录；
- 通过 `REGISTER_LOGIC(logic_xxx)` 自动注册，无需修改中央分发表；
- `ChannelContext` 提供帧、推理结果、ROI、时间、状态、参数、绘制和跨通道快照；
- `logic.json` 统一声明模块参数、Web 动作、上报字段及热重载策略；
- 每通道拥有独立的 `ctx->state`，同一逻辑可安全复用于多个通道；
- 支持业务按钮动作、系统级动作和跨通道全局逻辑。

### 显示、录像与上报

- HDMI/GTK 宫格显示；
- 内置 RTSP 服务，可输出与本地显示一致的拼接画面；
- ROI、检测框、姿态、分割和自定义绘制统一渲染；
- 告警图片与原始分辨率事件视频；
- 报警前后视频环形缓冲和异步 MP4 编码；
- 本地持久化事件发件箱，断网时保留并自动重试；
- 图片投递到业务服务器或 Dify，图片、视频和纯 JSON 事件可投递到 Dify；
- 每个 delivery 独立维护上传状态，全部成功后自动删除本地事件。

### Web 管理平台

- 可视化编排视频源、模型、ROI、业务逻辑、SOP 和上报策略；
- 应用包上传、安装、启动、停止和状态查看；
- 支持选择不同配置文件启动；
- 实时画面、运行日志和待上报记录查看；
- Web 按钮向指定通道业务逻辑发送动作；
- 浏览器终端、后台服务管理、上传连接和 OTA 配置；
- 配置保存后由 C++ 运行时自动检测并热更新。

## 系统架构

```mermaid
flowchart LR
    Web[React 可视化管理平台]
    API[FastAPI 后端]
    Config[JSON 配置与应用包]

    Capture[RTSP / File / USB]
    Inlet[帧入口与 FPS 节流]
    Infer[RKNN / RGA 推理引擎]
    Track[结果分发与跟踪]
    Logic[ChannelContext + logic_xxx]

    Display[HDMI / RTSP 输出]
    Alarm[图片与事件视频]
    Outbox[本地事件发件箱]
    Upload[统一上传服务]
    Remote[业务服务器 / Dify]
    OTA[模型 OTA 服务]

    Web --> API
    API --> Config
    Config --> Logic
    API -->|进程与通道控制| Logic

    Capture --> Inlet
    Inlet --> Infer
    Infer --> Track
    Track --> Logic
    Inlet --> Display
    Logic --> Display
    Logic --> Alarm
    Alarm --> Outbox
    Outbox --> Upload
    Upload --> Remote
    OTA -->|更新模型和配置| Infer
```

### 单帧运行链路

```text
GStreamer appsink
  → videoOutHandle
  → 事件视频源帧缓存
  → 每通道 FPS 节流
  → RGA 转换为模型输入图
  ├─ 无模型通道：同步执行传统 CV logic
  └─ 推理通道：任务队列 → RKNN worker → 同帧结果分发
       → tracker
       → 构造 ChannelContext
       → 执行动作处理器
       → 执行当前 logic_xxx
       → 原子写回 frame / results / state / draw commands
       → 显示、告警、录像和跨通道快照
```

业务 logic 和告警路径使用严格匹配的同帧图像与结果；实时预览优先显示最新解码帧，并使用最近结果进行叠加，以降低观看延迟。

## 仓库结构

```text
.
├── rk3588_yolo/                 # C++ 视觉分析运行框架与默认应用
│   ├── assets/                  # RKNN 模型、标签和示例配置
│   ├── scripts/                 # logic 清单生成与一致性校验
│   ├── src/
│   │   ├── capturer/            # GStreamer RTSP/File/USB 采集与重连
│   │   ├── analyzer/            # 帧入口、推理调度、跟踪和结果分发
│   │   ├── yolo/                # RKNN 模型实现与多模型组合
│   │   ├── logic/core/          # ChannelContext、注册表、参数和全局逻辑
│   │   ├── logic/modules/       # 可扩展业务逻辑模块
│   │   ├── player/              # HDMI 显示、叠加和 RTSP 输出
│   │   ├── alarm/               # 告警事件与图片发件箱生产端
│   │   ├── recorder/            # 报警前后事件视频
│   │   ├── control/             # Web/外部通道动作控制
│   │   ├── config/              # 配置解析、校验和热重载字段
│   │   └── core/                # 全局控制块与运行时基础能力
│   ├── tests/                   # C++ 单元测试
│   ├── CMakeLists.txt
│   └── build.sh                 # 编译、打包和部署脚本生成
├── web_console/
│   ├── frontend/                # React、TypeScript、XYFlow、Zustand
│   ├── backend/                 # FastAPI 管理 API 与 WebSocket
│   └── install.sh               # Web 控制台安装脚本
├── service/
│   ├── upload/                  # 可靠事件上传服务
│   └── model_update/            # 模型 OTA 服务
├── docs/                        # 开发、运维和模块文档
├── install_deps.sh              # RK3588 依赖安装
└── LICENSE                      # GPL-3.0
```

## 支持的模型与输入源

| 类别 | 当前支持 |
|---|---|
| 输入源 | `rtsp`、`file`、`usb` |
| 模型类型 | `yolov5`、`yolov8_det`、`yolov8_pose`、`yolov5_seg` |
| 模型格式 | Rockchip RKNN |
| 图像处理 | OpenCV、RGA |
| 解码与推流 | GStreamer、GStreamer RTSP Server |
| 本地显示 | GTK3 |
| 管理前端 | React、TypeScript、Vite、XYFlow、Zustand |
| 管理后端 | FastAPI、WebSocket |

新增模型类型目前需要实现 `ModelBase` 并在模型工厂中注册，然后重新编译主程序。

## 快速开始

### 1. 环境要求

- RK3588 / AArch64 Linux；
- Debian、Ubuntu、Armbian 或兼容发行版；
- 可用的 Rockchip RKNN Runtime 和 RGA；
- GStreamer 1.x；
- 带 `freetype` 模块的 OpenCV；
- 构建 Web 前端时需要 Node.js 18+。

### 2. 安装依赖

只运行预编译应用包：

```bash
bash install_deps.sh
```

需要在 RK3588 板端从源码编译：

```bash
bash install_deps.sh --build
```

Rockchip RKNN/RGA 的头文件和运行库通常由板卡 BSP、系统镜像或交叉编译环境提供。

### 3. 板端调试构建

```bash
cd rk3588_yolo
./build.sh --debug
./rk3588_yolo ./assets/config_sop.json
```

`--debug` 只生成可执行文件，不创建完整应用包。默认示例配置是 SOP 路径合规检测，请根据设备修改视频源、模型和标签路径。

### 4. 构建发布包

```bash
cd rk3588_yolo
./build.sh dist
```

脚本会自动判断构建方式：

- AArch64/ARM：在板端原生编译；
- x86_64：使用配置好的 `rk3588_builder` Docker 交叉编译镜像。

发布目录 `rk3588_yolo/dist/` 包含：

- `rk3588_yolo` 可执行程序；
- `assets/` 模型、标签和配置；
- `logics.json` Web 能力清单；
- `services/upload` 和 `services/model_update`；
- `run.sh`、`deploy.sh`、`stop.sh`、`setup_python.sh`。

前台运行发布包：

```bash
cd rk3588_yolo/dist
bash setup_python.sh
bash run.sh ./assets/config_sop.json
```

注册为 systemd 服务：

```bash
cd rk3588_yolo/dist
bash deploy.sh ./assets/config_sop.json
```

`deploy.sh` 会交互式选择是否启动视觉主程序、模型 OTA 和统一上传服务。

### 5. 安装 Web 管理平台

```bash
cd web_console
sudo bash install.sh
```

安装完成后访问：

```text
http://<RK3588-IP>:8080
```

控制台默认从 `/opt/ai_apps/` 扫描应用包。将刚构建的应用包安装到控制台：

```bash
cd rk3588_yolo
sudo ./install_app.sh dist
```

## 配置示例

配置采用 `schema_version = 2`。`stream.src_type` 必须显式指定，不能只依赖 URL 自动推断。

```json
{
  "schema_version": 2,
  "global": {
    "enable_display": false,
    "enable_rtsp": true,
    "disp_width": 1280,
    "disp_height": 720,
    "tile_cols": 1,
    "tile_rows": 1,
    "max_fps": 25,
    "queue_size": 1,
    "obj_thresh": 0.3,
    "nms_thresh": 0.45
  },
  "channels": [
    {
      "id": 0,
      "enable": true,
      "infer_enable": true,
      "stream": {
        "src_type": "rtsp",
        "url": "rtsp://192.168.1.10/live",
        "video_enc": "h264"
      },
      "model_type": "yolov8_det",
      "model_path": "assets/yolov8n.rknn",
      "label_path": "assets/labels.txt",
      "logic": "logic_upload",
      "logic_parameters": {},
      "roi_zones": [
        {
          "name": "entrance",
          "polygon": [[0.1, 0.1], [0.9, 0.1], [0.9, 0.9], [0.1, 0.9]]
        }
      ],
      "report_policy": {
        "enabled": false,
        "deliveries": []
      }
    }
  ]
}
```

通道 `id` 是运行时、Web API、告警、录像和控制动作共同使用的唯一身份，必须唯一且位于 `[0, 15)`。

## 开发新的通道逻辑

每个通道逻辑由一个 C++ 入口和一个模块清单组成：

```text
rk3588_yolo/src/logic/modules/logic_people_count/
├── logic.cpp
└── logic.json
```

最小 `logic.cpp`：

```cpp
#include "logic/core/logic_common.h"

static void logic_people_count(ChannelContext *ctx)
{
    const int count = ctx->target_count("person");
    draw_text(ctx,
              ("person: " + std::to_string(count)).c_str(),
              cv::Point(20, 40),
              cv::Scalar(0, 255, 0));
}

REGISTER_LOGIC(logic_people_count);
```

最小 `logic.json`：

```json
{
  "label": "人员计数",
  "parameters": {
    "type": "object",
    "additionalProperties": false,
    "properties": {}
  },
  "report_fields": []
}
```

新增模块后重新运行 CMake 或 `build.sh`。构建过程会：

1. 递归收集 `src/logic/modules/` 中的 C++ 源文件；
2. 从 `REGISTER_LOGIC()` 获取唯一 logic ID；
3. 校验 `logic.json`、参数访问器和热重载策略；
4. 将 Schema 嵌入二进制；
5. 在发布包中生成供 Web 使用的 `logics.json`。

普通模块参数应声明在 `logic.json.parameters.properties`，运行时通过以下接口读取：

- `ctx->param_float()`；
- `ctx->param_int()`；
- `ctx->param_bool()`；
- `ctx->param_string()`；
- `ctx->param_json()`。

不要使用函数内 `static` 保存每通道状态；跨帧状态应保存在 `ctx->state`。

## 内置通道逻辑

| Logic ID | 作用 |
|---|---|
| `logic_path_sop` | SOP 路径、顺序、停留、分支、环路和耗时合规检测 |
| `logic_upload` | ROI 目标告警与统一上报示例 |
| `logic_upload_teach` | 统一图片/视频/JSON 上报教学示例 |
| `logic_button_demo` | Web 按钮和系统动作演示 |
| `logic_periodic_snapshot_demo` | 周期截图与参数热重载演示 |
| `logic_save_frame_pair` | 保存原始分辨率帧和模型输入帧 |
| `logic_default` | 可删除的空白逻辑示例 |
| `logic_course_01` | 文字叠加课程示例 |
| `logic_course_02` | `ChannelContext` 使用课程示例 |

通道可以不配置 `logic`。此时仍会执行视频、模型、跟踪和通用绘制管线，但不会调用业务后处理模块。

## 配置热重载

运行时监控当前配置文件，等待文件写入稳定后重新解析。当前支持：

- 阈值、检测类别、队列深度和跟踪参数更新；
- ROI 和 logic 参数更新；
- logic 切换及状态安全清理；
- 模型热替换；
- 视频源地址和 USB 参数切换；
- 全局逻辑实例重启；
- 模块参数的 `preserve_state`、`reset_state`、`restart_required` 策略。

以下变化涉及固定运行拓扑，热重载会拒绝并要求重启：

- 通道数量、ID 或启用状态变化；
- HDMI/RTSP 输出开关、布局、端口、编码等输出拓扑变化。

配置采用不可变运行快照发布。业务帧持有快照后，本帧所见的通道配置、ROI 和模块参数保持一致。

## 告警事件与可靠上传

业务逻辑只调用统一入口：

```cpp
report_alarm(ctx, "person_enter", "检测到人员进入", {
    alarm_field("label", "person"),
    alarm_field("count", 1),
});
```

媒体类型、叠加方式、视频前后时间窗、接收端和字段映射由 Web 保存的 `report_policy` 决定。

事件链路：

```text
channel logic
  → alarm_store/<event_id>/manifest.json
  → snapshot.jpg / raw.jpg / clip.mp4
  → unified_upload 扫描
  → delivery 独立上传和重试
  → 全部成功后删除事件目录
```

默认发件箱上限为 1 GiB，并保留至少 512 MiB 可用磁盘空间。可以通过以下环境变量覆盖：

- `ALARM_STORE_DIR`；
- `ALARM_STORE_MAX_BYTES`；
- `ALARM_STORE_MIN_FREE_BYTES`。

## 一致性检查

校验所有 logic 注册、模块清单、参数 Schema 和 C++ 参数访问器：

```bash
cd rk3588_yolo
python3 scripts/generate_logics_catalog.py --check
```

编译后查看二进制实际注册的通道逻辑：

```bash
./rk3588_yolo --list-logics
```

构建 Web 前端：

```bash
cd web_console/frontend
npm install
npm run build
```

## 项目边界

- 通道 logic 是编译期模块，不是运行时动态加载的 `.so` 插件；
- Web 图编辑器编排固定的视觉分析角色，不执行任意用户定义 DAG；
- 模型类型由 C++ 模型工厂注册，新增类型需要重新编译；
- 全局 logic 当前仍采用中央注册方式；
- `logic_path_sop` 是默认业务应用，不是框架运行所必需的兜底逻辑。

## 文档

- [文档总入口](docs/README.md)
- [二次开发课程大纲](docs/二次开发课程大纲.md)
- [通道逻辑开发指南](docs/skills/rk3588-channel-logic/SKILL.md)
- [全局逻辑开发指南](docs/skills/rk3588-global-logic/SKILL.md)
- [控制台、部署与运维指南](docs/skills/rk3588-console-ops/SKILL.md)
- [源码模块索引](docs/skills/rk3588-src-modules/README.md)

如文档与当前实现存在差异，以源码、头文件、模块 `logic.json`、前端序列化和后端路由为准。

## License

本项目致敬GNU计划，基于 [GNU General Public License v3.0](LICENSE) 开源。
