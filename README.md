# RK3588 多路视觉分析框架

在一块 RK3588 上同时接入多路视频（RTSP / 本地文件 / USB），用 NPU 跑 YOLO 检测/分割/姿态，按「每通道一段自行编排的业务逻辑」做入侵、计数、跌倒等视觉分析并支持上报服务器/dify，全程可在网页上拖拽配置、热重载、看实时画面。本项目架构的设计适合二次开发，上层的逻辑编排部分与底层代码解耦。二次开发者只需要关注逻辑的实现，不用关注底层的视频取流，解码，NPU推理，多线程调度等内容。本项目配有基础的使用教程，开发者经过短暂的学习即可自行编写出有趣的功能。docs文件夹中也配有看具体的模块说明供开发者参考，也可作为供大模型参考的提示词。

如有任何疑问或意见可随时联系作者：Sunny_Wei Email:1927096839@qq.com

祝编程愉快！ Happy coding! :)

---

## 特性

- **多路并发推理**：单 batch + 多线程，使用yolov5n.rknn推理时，9路视频可达25帧，15路约 11–12 fps（接近 Jetson Orin Nano + DeepStream 的水平）。
- **三种视频源**：RTSP 网络流、本地视频文件、USB 摄像头，并支持自动重连。
- **支持多种模型**：YOLOv5 / YOLOv8 检测、YOLOv8-Pose 关键点、YOLOv5-Seg 分割（`.rknn`）。
- **支持传统cv算法**：可对视频帧直接处理后显示在视频窗口。
- **可插拔业务逻辑**：每个逻辑一个 `logic_xxx.cpp` 文件，自注册，新增/删除一个功能只动少量文件。
- **模块自由组合**: 视频流模块，yolo模块，逻辑函数模块，告警上报服务自由组合。但需要自行编写的逻辑函数中支持，可参考已有示例。
- **可视化 Web 控制台**：拖拽配置视频源/模型/ROI/逻辑/上报，生成 `config.json`，一键启动、看实时画面、查日志。
- **热重载**：阈值、类别、逻辑名、可调参数、模型路径改了即生效，无需重启程序。
- **异步上报 + 断网不丢**：server 告警由 C++ 落盘到本地「发件箱」目录（`alarm_store/`），Python 微服务扫描补传到业务服务器——**断网时积累在本地、重连后逐条补发、发成功即删**，这条 store-and-forward 链路保证边缘侧与服务器断连期间报警不丢失（落盘方案，断电重启也不丢）；Dify 分析则走 Redis 队列。两条路都非阻塞，不拖慢推理线程。

---

## 项目结构

```
.
├── install_deps.sh        # 一键安装运行/编译依赖（apt + Node + pip + Redis）
├── rk3588_yolo/           # C++ 主程序（推理引擎 + 解码 + 显示 + 业务逻辑）
│   ├── build.sh           #   编译打包（板端原生 / Docker 交叉）
│   ├── install_app.sh     #   把产物装进控制台目录 /opt/ai_apps/
│   └── src/               #   源码（各模块说明见 docs/skills/rk3588-src-modules/）
├── web_console/           # Web 配置控制台（FastAPI 后端 :8080 + React 前端）
│   └── install.sh
├── service/               # Python 微服务
│   ├── upload/            #   告警上报（扫本地发件箱 → HTTP；消费 Redis → Dify）
│   └── model_update/      #   OTA 模型在线更新
└── docs/                  # 二次开发文档 / 技能说明 / 开发日志
```

---

## 环境要求

- **硬件**：Rockchip RK3588（3 个 NPU 核心）。
- **系统**：Debian / Ubuntu / Armbian（aarch64）。推荐 Debian 。
- **网络**：能联网装依赖（脚本已内置国内镜像回退）。
- **其它**：Redis（脚本会装并启动）；如需网页前端构建需 Node 18+（脚本会装）。
- **模型**：NPU 只能跑 `.rknn`。仓库 `rk3588_yolo/assets/` 自带若干示例模型（yolov8n / yolov5s / pose / seg 等）；自训模型需 `pt → onnx → rknn` 转换后放进 `assets/`。

> 仓库不含预编译产物（`build/`、`dist/` 已在 `.gitignore`），克隆后需自行编译。

---

## 快速开始（在你的 RK3588 上）

以下命令都在板子上执行。

### 0. 克隆

```bash
git clone https://github.com/LaoWei2001/rk3588_visual_analysis_framework.git
# 赋予整个文件夹所有内容读写权限
chmod 777 -R rk3588_visual_analysis_framework-main
cd rk3588_visual_analysis_framework-main
```

### 1. 装依赖

```bash
# 若要在板端从源码编译主程序，就带 --build（含 OpenCV/GTK 等 -dev 包）
./install_deps.sh --build
```
> 如果你只想运行别人给的预编译 `dist` 包，不编译，则去掉 `--build`。脚本会装 apt 运行库、Node、Redis 和所有 `requirements.txt`。

### 2. 编译主程序

```bash
cd rk3588_yolo
./build.sh dist          # 自动识别 aarch64 → 板端原生编译，产物输出到 ./dist/, 名字可自行更改。   
```
`dist/` 里是一个自包含程序包：可执行文件 `rk3588_yolo` + `assets/`（模型/标签）+ 依赖库 `libs/` + `run.sh` / `deploy.sh` 等脚本。

### 3. 安装并打开 Web 控制台

```bash
cd ../web_console
./install.sh                       # 装到 /opt/ai_apps/_console，构建前端
```
浏览器打开 **`http://<板子IP>:8080`** 即可进入控制台。

### 4. 把程序装进控制台

```bash
cd ../rk3588_yolo
# 将之前编译产出的程序包复制到板端的/opt/ai_apps/中, web端会自动识别到程序包
./install_app.sh dist
```

### 5. 在控制台里配置并启动

1. 在「程序管理」找到刚装的程序，点 **配置** 进入画布。
2. 拖拽节点配置：**视频流**（RTSP/文件/USB）→ **模型**（选 `assets/` 下的 `.rknn`）→ **ROI 区域** → **逻辑函数** →（可选）**上报配置**，保存即生成 `config.json`。
3. 回到「程序管理」点 **启动**，再点 **实时画面** 看效果。

完成。后续改配置/换逻辑/调参数大多可热重载，无需重编译。

---

## 不用web控制台？板端命令行也能直接跑，查看效果需要连接显示器

`build.sh` 产出的 `dist/` 也能脱离控制台独立运行（需先准备一份 `config.json`，最简单是用控制台生成后拷出来，或参考 `docs/` 自己写）：

```bash
cd rk3588_yolo/dist
./run.sh ./assets/config.json      # 前台运行 + 显示窗口（接显示器/SSH X11）
```

正式后台部署（注册 systemd 服务：主程序 + 上报 + OTA，交互式选择启用）：

```bash
./deploy.sh ./assets/config.json
# 之后用 journalctl -u vision_app -f 看实时输出
```

---

## 上报与微服务（可选）

业务逻辑里调用 `alarm_uploader_enqueue(...)` / `dify_uploader_enqueue(...)` 时，C++ 非阻塞投递，由 `service/upload` 微服务异步处理：

- **server 告警** → C++ 落盘到本地发件箱 `alarm_store/`（带框图 + 原图 + `.json` 元数据），`OutboxForwarder` 扫描 → HTTP POST 到业务服务器 → **成功即删、断网攒着、重连补发**（不经 Redis，断电重启也不丢）。
- **dify 分析** → C++ `redis_rpush("dify_queue")` → `DifyUploader` 消费 → 上传图片 + 触发 Dify 工作流（可在网页填提示词，让大模型做二次核验）。

上报地址按通道走（在网页「上报配置」节点填），留空则回落到 `service/upload/config.yaml` 的默认值。微服务由 `deploy.sh` 一并注册为 systemd 服务。

---

## 二次开发(未来打算编写简单的教程)

- **加一个通道逻辑**：在 `rk3588_yolo/src/logic/` 新建 `logic_xxx.cpp`（顶部 `#include "logic_common.h"`，实现 `static void logic_xxx(ChannelContext*)`，文件末尾 `REGISTER_LOGIC("logic_xxx", logic_xxx);`），重新 `build.sh` 即可。删除功能＝删除该文件。
- **加可热重载参数**：`config.h` 的 `ChannelConfig` 加字段 + `config_init.cpp` 用 `REG_C` 注册 + `logics.json` 声明，网页自动渲染。
- **完整指南**：见 `docs/skills/rk3588-channel-logic/`（含 `ChannelContext` API 速查与每个现成逻辑的真实代码示例）。各源码模块（`analyzer/` `capturer/` `config/` `core/` `logic/` `player/` `uploader/` `yolo/`）的说明文档统一收在 `docs/skills/rk3588-src-modules/`。

---

## 常见问题

- **网页打不开 / 看不到程序**：确认 `rk3588-console` 服务在跑（`systemctl status rk3588-console`），程序已 `install_app.sh` 进 `/opt/ai_apps/`。
- **实时画面是黑的 / 无视频流**：监看仅在程序运行时可用；要用网页看画面需在「全局配置」勾选 **RTSP 推流** 并重启程序。
- **ROI 在画面上偏位**：ROI 与检测框统一在模型输入坐标系（640）。USB 摄像头高帧率会绑定低分辨率导致偏移——显式设置采集分辨率，或降低最大 FPS。
- **类别显示错误**：`r.label` 字符串必须和该模型 `labels.txt` 里的类名完全一致。
- **改了配置不生效**：通道数量、流地址、显示分辨率、`infer_enable` 等需停止再启动；阈值/逻辑名/可调参数支持热重载。
- **跌倒等姿态逻辑不准**：建议用 `yolov8_pose` 模型（有 17 关键点）。

更多踩坑记录见 `docs/development_log.md`。

---

## 许可证

本项目向GNU计划致敬，采用 **GNU General Public License v3.0 (GPL-3.0)** 许可。

```
基于RK3588的多路视觉分析框架
/*
 * Copyright (C) 2026 Sunny_Wei
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
```
完整的许可证文本见仓库根目录的 [LICENSE](LICENSE) 文件。