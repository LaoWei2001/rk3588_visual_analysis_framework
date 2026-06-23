---
name: rk3588-console-ops
description: >-
  Operate, deploy, and troubleshoot the RK3588 vision system's **web console**,
  **deployment pipeline**, and **background services** (NOT for writing channel
  logic — that's the rk3588-channel-logic skill). Use this skill whenever the
  task is about: 部署到一台新 RK3588 板子 / installing dependencies (apt/node/pip)
  / build.sh / install_app.sh / 上传或删除程序包 / the web console (FastAPI 后端 +
  React 编辑器，rk3588-console.service) / 启停后台服务 (ota_agent / unified_upload,
  systemd, systemctl, journalctl) / 服务起不来报错 (CHDIR, Failed at step, unit
  路径失效) / 服务配置 (config.yaml / ota_config.json) / OTA 升级服务 / Redis 队列 /
  USB ROI 偏移 / 改了 C++/控制台代码不生效怎么重新部署. Trigger on any deploy/ops/console
  question for this project, even if phrased casually ("板子上服务起不来", "怎么把程序
  装上去", "网页打不开"). Do NOT use for writing/editing logic_xxx detection rules.
---

# RK3588 控制台 / 部署 / 运维

把这套 RK3588 视觉系统部署起来、用网页控制台管起来、出问题能定位。**写检测/报警逻辑不归这——那是 `rk3588-channel-logic` skill。**

## 一、系统由哪些部分组成（谁管谁，先建立全貌）

```
板子 (/opt/ai_apps/ 是控制台的"程序根")
├── _console/                     Web 控制台本体（FastAPI 后端 + React 前端）
│      └ systemd: rk3588-console.service（User=root，:8080）
├── <App1>/  <App2>/ ...          每个"程序包"(build.sh 产物 install 进来的)
│   ├── rk3588_yolo               C++ 推理二进制 ← 控制台 process_manager 用 subprocess 启停
│   ├── assets/  (config.json, roi_zones.json, *.rknn)
│   └── services/                 两个 Python 微服务（随包带）
│       ├── model_update/ ota_agent.py + ota_config.json   ← systemd: ota_agent.service
│       └── upload/       main.py + config.yaml             ← systemd: unified_upload.service
└── (Redis 在 127.0.0.1:6379，上报消息总线)
```

**三种进程、三套托管方式**（容易混，记牢）：
| 进程 | 谁托管 | 怎么启停 |
|------|--------|---------|
| C++ 推理二进制 (vision_app) | 控制台 `process_manager`（subprocess） | 网页「程序管理」▶启动/■停止 |
| Web 控制台自身 | systemd `rk3588-console.service` | `systemctl` / `web_console/install.sh` / `stop.sh` |
| 两个 Python 微服务 | systemd `ota_agent` / `unified_upload` | 网页「后台服务」面板 或 命令行 `systemctl`（同一套单元） |

> 后台服务的启停机制、网页↔板端怎么配合、板端直接启停命令、CHDIR 排错——全在 **`references/services-upload-and-ota.md`**（含微服务架构 + §7 启停配合 + 板端命令）。涉及后台服务的问题先读它。

## 二、部署一台新板子（标准流程）

```bash
# 0. 装依赖（apt + Node + pip 一键；国内网络已适配：Node 走 npmmirror tarball、apt backports 404 容错）
bash install_deps.sh              # 运行时依赖；加 --build 再装板端从源码编译 C++ 的 -dev 包

# 1. 编译 C++ + 打包（产出 dist/：二进制 + assets + libs + services + deploy.sh 等）
cd rk3588_yolo && ./build.sh dist

# 2. 把程序包装进控制台（复制 dist → /opt/ai_apps/dist/，重名会问覆盖/改名）
sudo ./install_app.sh dist        # 之后该 App 在网页「程序管理」就能看到

# 3. 部署 Web 控制台（→ /opt/ai_apps/_console，装并起 rk3588-console.service）
cd ../web_console && bash install.sh
#    访问 http://板子IP:8080（SSH 账号密码登录）

# 4. 起后台服务（任选其一）
#    a) 网页「后台服务」面板：选 App → 安装 → 启动
#    b) 命令行一键：在 dist 目录 bash deploy.sh ./assets/config.json
```

**改了东西后怎么重新生效**（高频）：
- 改了 **C++**（逻辑/上报/ROI 等源码）→ 必须 `./build.sh dist && sudo ./install_app.sh dist`，再在网页重启该程序。
- 改了 **控制台代码**（后端路由/前端）→ `cd web_console && bash install.sh` 重部署（会重 build 前端 + 重启 rk3588-console）。
- 改了 **某 App 的 config.json/服务配置** → 网页保存即可；config.json 由 C++ 热重载（ROI 例外，要重启程序）；服务配置要把对应后台服务停止再启动。

## 三、网页控制台能干什么（功能 → 后端路由 → 落盘位置）

| 网页功能 | 后端路由 | 落到哪 |
|---------|---------|--------|
| 程序列表/启停/监看 | `apps.py` / `process.py`(process_manager) / `stream.py`(MJPEG) | 进程；`run.pid` |
| **上传/删除程序包** | `apps.py`(`POST /apps/upload`,`DELETE /apps/{name}`) | `/opt/ai_apps/<App>/` |
| 配置编辑器（画布）保存 | `config_io.py` | `<App>/assets/config.json` |
| ROI 绘制 | `config_io.py`(`/roi`) | `<App>/assets/roi_zones.json` |
| 「服务配置」弹窗 | `upload_config.py` / `ota_config.py` | `<App>/services/upload/config.yaml` / `…/model_update/ota_config.json` |
| 「后台服务」面板（装/启停/健康/日志） | `services.py` | systemd 单元 + `systemctl`/`journalctl` |
| 登录（SSH 账号） | `auth.py`(PAM) | — |

## 四、常见运维问题（速查：现象 → 原因 → 修）

> 下面是**已知症状的速查**。遇到没列出的问题、或要系统地定位（各部分日志在哪、前端/后端/板端二分法、"改了不生效"自查清单、以及 Web 终端 xterm/PTY/输入法/HTTPS 的完整实战案例），看 **`references/debugging-playbook.md`**。

- **后台服务起不来 `CHDIR ... No such file or directory` / `Failed at step CHDIR`**：单元里的 `WorkingDirectory` 指向的目录不存在。多为**残留旧单元**（如旧的 `deploy.sh` 装的，指向已删的 `/userdata/.../dist/...`）或该 App 没带 `services/`。**网页「后台服务」面板会自动把这种单元标成「⚠ 路径失效」**，选当前 App 点「🔧 修复并启动」即可——后端强制重写单元指向 `/opt/ai_apps/<App>/services/...` 并直接启动，不用手动 rm。命令行核对：`systemctl cat ota_agent | grep WorkingDirectory` → `ls -ld 那个路径`。详见 `references/services-upload-and-ota.md` §7。
- **USB 摄像头 ROI 偏移、且不同 max_fps 视野不同**：USB 帧率绑分辨率（fps 档→不同分辨率+不同视野），ROI 抓帧分辨率与推理实际跑的对不上。修：视频流节点把「采集分辨率」设成**固定值**（方案B，与 fps 解耦），重画 ROI。
- **改了 ROI 不生效**：ROI 只在程序**启动时**加载（`load_roi_zones`），不热重载。改完要在「程序管理」**停止再启动**。
- **OTA 升级后模型没换上 / 推理还是旧模型**：`ota_config.json` 的 `target_config` 必须等于 C++ 实际在跑的那份配置（默认 `config.json`）；改的不是跑的那份就热重载不进去。
- **`apt update` 报 `bullseye-backports ... 404`**：失效的第三方源。`install_deps.sh` 已用 `|| true` 容错不致命；要根治就注释掉 `/etc/apt/sources.list` 里那行。
- **国内装 Node 失败/超时**：`install_deps.sh` 已优先用 npmmirror 预编译 tarball；手动可 `npm config set registry https://registry.npmmirror.com`。
- **网页改了配置但服务/程序行为没变**：分清谁热重载——config.json 的普通字段 C++ 热重载；ROI 要重启程序；服务配置（config.yaml/ota_config.json）要重启对应后台服务。
- **网页打不开**：`systemctl status rk3588-console`；`journalctl -u rk3588-console -n 50`。aarch64 上 `uvicorn[standard]` 偶发要编译，失败可退精简 `uvicorn fastapi`。

## 五、关键文件 / 脚本地图

| 路径 | 作用 |
|------|------|
| `install_deps.sh`（仓库根） | apt + Node + pip 一键装依赖（`--build` 加编译依赖） |
| `rk3588_yolo/build.sh` | 编译 C++ + 打包 dist（含 services/、生成 run.sh/deploy.sh/setup_python.sh） |
| `rk3588_yolo/install_app.sh` | 把 dist 复制进 `/opt/ai_apps/<名>/` 让控制台识别 |
| `web_console/install.sh` | 部署控制台（后端+前端→`/opt/ai_apps/_console`，装并起 `rk3588-console.service`） |
| `web_console/stop.sh` | 停控制台（`systemctl stop rk3588-console`；`--disable` 取消自启） |
| `dist/deploy.sh`（build 产物） | 交互式生成并注册 `vision_app`/`ota_agent`/`unified_upload` 三个 systemd 单元 |
| `web_console/backend/routers/` | 控制台后端各路由（见上表） |
| `web_console/rk3588-console.service` | 控制台 systemd 单元模板 |

## 六、二次开发指引
- **控制台前端加页面/功能**（React 架构、目录职责、端到端加页面/接口、WebSocket 用法、改动如何生效、坑）：见 **`references/web-console-frontend.md`**。
- **加后台微服务 / 加服务配置路由 / 加后台服务面板项**：见 `references/services-upload-and-ota.md` §6。
- **控制台后端加功能**：在 `web_console/backend/routers/` 加路由，`main.py` 注册（`/api` 前缀，鉴权由全局中间件覆盖；WebSocket 路由自带 `/ws` 前缀）；前端配套见上条。
- **改部署/依赖**：动 `install_deps.sh` / `build.sh`（打包逻辑）/ `install_app.sh`。
- **出问题怎么定位**：见 **`references/debugging-playbook.md`**（含"改了不生效"自查 + 二分定位法 + 终端案例）。
