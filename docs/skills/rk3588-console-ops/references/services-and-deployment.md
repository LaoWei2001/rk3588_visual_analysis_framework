# 服务与部署

## 目录

- [构建一个完整视觉 App](#构建一个完整视觉-app)
- [安装 App 与控制台](#安装-app-与控制台)
- [运行模式](#运行模式)
- [两个后台服务](#两个后台服务)
- [上传服务运行数据](#上传服务运行数据)
- [OTA 当前契约](#ota-当前契约)
- [什么改动怎样生效](#什么改动怎样生效)

## 构建一个完整视觉 App

```bash
cd vision_analysis
python3 scripts/generate_logics_catalog.py --check
./build.sh my_app
```

`build.sh my_app` 按主机架构选择板端原生或 Docker 交叉编译，并把二进制、assets、依赖库、两项
Python 服务、生成的 `logics.json` 和 `report_templates/` 放到当前 `vision_analysis/` 下的 `my_app/`。

快速调试：

```bash
./build.sh --debug
```

该模式只生成 Debug 二进制到 `vision_analysis/`，不产生可安装完整包。输出目录名必须是单层名字；
`--clean` 会清除构建缓存后重编。

## 安装 App 与控制台

命令行安装包：

```bash
cd vision_analysis
sudo ./install_app.sh my_app
```

目标默认 `/opt/ai_apps/my_app`。同名覆盖会删除旧 App 目录后复制新包，但不会清理包外的
`/opt/ai_apps/.data/my_app`。

部署控制台：

```bash
cd web_console
bash install.sh
```

默认安装到 `/opt/ai_apps/_console`，生成并启用 `rk3588-console.service`，端口 8080。可在命令前
设置 `APPS_ROOT`/`INSTALL_DIR` 更改位置。脚本有 Node/npm 时重新构建前端；没有时使用源码目录已有
的 `frontend/dist`，两者都不存在则失败。

Web“上传程序”与 `install_app.sh` 写入同一 App 根，但 Web 接受归档并做路径安全检查。Web 上传时
会停止全部托管视觉 App，避免替换正在执行的文件。

## 运行模式

Web 通过 `systemd-run --pipe` 启动二进制，工作目录是 App 根，参数是所选 `assets/<config>.json`。
它设置：

- `RK_LOGIC_CONTROL_SOCKET=<App>/run.control.sock`；
- `ASSETS_DIR=<App>/assets`；
- `EVENT_STORE_DIR=<APPS_ROOT>/.data/<App>/event_store`；
- `LD_LIBRARY_PATH=<App>/libs...`；
- 存储管理器生成的运行环境。

部署模式把 `global.enable_display` 原子写为 0；调试模式写为 1，并补 `DISPLAY=:0` 与可能的
Xauthority。`run.pid/run.mode/run.config/run.started_at/run.systemd_unit` 是运行标记，不是源码配置。

## 两个后台服务

Web 服务页只管理：

| key | unit | 包内工作目录 |
|---|---|---|
| `unified_upload` | `unified_upload.service` | `<App>/services/upload` |
| `ota_agent` | `ota_agent.service` | `<App>/services/model_update` |

启动/重启服务前必须先启动视觉 App。Web 会在运行锁内确认当前 App、初始化 `.data/<App>`、重写
systemd unit 并重启。App 切换后，正在运行或设置了自启意图的服务会重新绑定新 App。

服务 unit 本身被 `systemctl disable`；开机恢复由 `rk3588-console` 根据持久化意图先恢复视觉 App，
再恢复服务，避免 unit 脱离上下文启动到旧目录。

常用只读命令：

```bash
systemctl status rk3588-console.service --no-pager
systemctl status unified_upload.service --no-pager
systemctl status ota_agent.service --no-pager
systemctl cat unified_upload.service
systemctl cat ota_agent.service
journalctl -u rk3588-console.service -n 200 --no-pager
journalctl -u unified_upload.service -n 200 --no-pager
journalctl -u ota_agent.service -n 200 --no-pager
```

## 上传服务运行数据

`service/upload/main.py` 的独立运行默认数据目录是 App 根 `.runtime`；Web unit 明确传入：

```text
UPLOAD_DATA_DIR=<APPS_ROOT>/.data/<App>
EVENT_STORE_DIR=<APPS_ROOT>/.data/<App>/event_store
```

服务每轮读取 `connections.yaml` 和活动/历史契约，因此连接/契约保存后不要求为“读取配置”专门重启；
但服务代码、unit 绑定或环境变更需要重启。

## OTA 当前契约

Web 启动 OTA 时设置：

- `ASSETS_DIR=<App>/assets`；
- `CONFIG_FILE=<当前视觉 App 的实际配置文件名>`；
- `OTA_CONFIG_FILE=<APPS_ROOT>/.data/<App>/ota_config.json`。

当前 `ota_agent.py` 要求后两者存在，不再自行用 `target_config=active` 猜配置。`ota_config.json`
必须提供非空 `platform_ws_host`（也可由 `PLATFORM_WS_HOST` 环境变量覆盖）。

OTA 指令按 `channels[].id + models[].id` 定位；`channel` 缺失时当前代码默认 0。调度入口要求
`model_id/version`，更新 worker 还要求非空 `type` 和字符串 MD5，并在下载后核对 MD5。当前代码没有
单独校验 `url` 格式，也不验证 `type` 是否属于 C++ 支持的模型类型，而是用平台 host 与 `url` 直接
拼 HTTPS 下载地址。下载的新 RKNN 写到 assets，以带 channel/model/hash 的文件名保存，然后更新当前
绑定配置中的 model_path、version、model_type；旧模型文件保留。C++ 配置热重载是否成功必须再查
视觉程序日志；OTA 已写配置/反馈成功不等于新模型一定加载成功。

## 什么改动怎样生效

| 改动 | 生效动作 |
|---|---|
| C++、logic manifest、模块模板、打包服务代码 | 重建完整包、重新安装、重启 App/相关服务 |
| Web 前端/后端 | 重新运行 `web_console/install.sh` |
| 当前运行 JSON 普通可热更字段 | Web 保存后观察 C++ config monitor；被拒绝时重启 App |
| `connections.yaml`、活动契约 | 上传 worker 下一轮重新加载；测试仍需真实事件 |
| `ota_config.json` | 重启 OTA 服务，因为模块启动时读取 |
| systemd WorkingDirectory/环境 | 从 Web 对当前 App 执行启动或重启，触发 unit 重写 |
