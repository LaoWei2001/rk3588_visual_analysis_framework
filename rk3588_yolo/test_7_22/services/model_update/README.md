# 模型 OTA 升级服务 (services/model_update/)

> 边缘端 OTA 守护进程，通过 WebSocket 与云端平台保持长连接，接收模型升级指令后自动下载、校验、替换模型文件，并配合主程序的热重载机制实现无停机模型升级。

---

## 文件清单

| 文件 | 职责 |
|------|------|
| `ota_agent.py` | OTA 守护进程主程序：WebSocket 连接管理、指令解析、下载校验、配置更新 |
| `requirements.txt` | Python 依赖 |

---

## 架构设计

```
云端平台（WebSocket Server）
    │  wss://tunnel.memanager.cn/ws/device/{DeviceID}
    │
    │  指令：{ "action": "UPDATE_COMMAND", "channel": 0,
    │           "version": "1.2", "url": "/models/xxx.rknn",
    │           "md5": "abc123...", "type": "yolov5" }
    ▼
ota_agent.py（asyncio 事件循环）
    │
    ├─ 1. 版本比对（读本地 config.json 中的 version 字段）
    │       └─ 版本一致 → 直接反馈成功，跳过下载
    │
    ├─ 2. 反馈"开始下载"（status=0）
    │
    ├─ execute_update()（独立线程，不阻塞事件循环）
    │       ├─ HTTPS 下载 .rknn 文件到 /tmp/
    │       ├─ MD5 校验
    │       ├─ 移动到 assets/ 目录（新文件名含 MD5 前 8 位）
    │       ├─ 更新 config.json 中对应通道的 model_path/model_type/version
    │       └─ 反馈成功（status=1）或失败（status=-1）
    │
    └─ 主程序热重载（config.json 变化 → config_monitor_thread 检测到 →
                     algorithm_reload_channel_model() 加载新模型）
```

**无停机升级**：升级期间旧模型继续推理，直到 `config.json` 写入完成后，C++ 主程序的配置热监控线程在下一个 2 秒周期自动探测到文件变化，热加载新模型，中断时间仅为一次模型初始化（通常 < 3 秒）。

---

## DeviceID 生成规则

DeviceID 由物理网卡 MAC 地址经 MD5 哈希生成（大写十六进制字符串），用于在平台上唯一标识设备：

```python
raw_mac = uuid.UUID(int=uuid.getnode()).hex[-12:].upper()  # 取最后 12 位（物理 MAC）
device_id = hashlib.md5(raw_mac.encode('utf-8')).hexdigest().upper()
```

---

## 配置文件约定

OTA 服务直接读写主程序的配置文件（**默认 `assets/config.json`**，可在 `ota_config.json` 或网页控制台「⚙ 服务配置」里改）。通道通过 `id` 字段匹配（不是数组下标），因此配置文件中每个通道必须有唯一的 `id`：

```json
{
  "channels": [
    {
      "id": 0,
      "model_path": "assets/model_ch0_f537e873.rknn",
      "model_type": "yolov5",
      "version": "1.1"
    }
  ]
}
```

OTA 升级后，该通道的配置会更新为：

```json
{
  "id": 0,
  "model_path": "assets/model_ch0_abcd1234.rknn",
  "model_type": "yolov5",
  "version": "1.2"
}
```

旧模型文件**保留不删除**（避免回滚需求），需手动清理。

---

## 资产目录自动检测

`ASSETS_DIR` 按以下优先级确定：

1. 环境变量 `ASSETS_DIR`（推荐生产部署使用）
2. 自动检测：从脚本路径向上查找 `rk3588_yolo/assets`（源码目录调试用）
3. 降级：从脚本路径向上查找 `assets`（dist 部署）

---

## 部署与运行

### 安装依赖

```bash
cd dist/services/model_update
pip3 install -r requirements.txt
```

### 直接运行

```bash
ASSETS_DIR=/path/to/dist/assets python3 ota_agent.py
```

### 修改平台 WebSocket 地址 / 目标配置文件

不再改源码。优先级：**环境变量 > `ota_config.json` > 默认值**。

`ota_config.json`（与本服务同目录，网页控制台「⚙ 服务配置」可直接写）：

```json
{
  "platform_ws_host": "tunnel.memanager.cn",
  "target_config": "config.json"
}
```

连接地址格式为：`wss://{platform_ws_host}/ws/device/{DeviceID}`

> 对应环境变量：`PLATFORM_WS_HOST`、`CONFIG_FILE`（覆盖 target_config）。

---

## WebSocket 协议说明

### 平台 → 设备（升级指令）

```json
{
  "action":  "UPDATE_COMMAND",
  "channel": 0,
  "version": "1.2",
  "url":     "/api/models/abc.rknn",
  "md5":     "d41d8cd98f00b204e9800998ecf8427e",
  "type":    "yolov5"
}
```

| 字段 | 说明 |
|------|------|
| `channel` | 目标通道 ID（与 config.json 中的 `id` 对应） |
| `version` | 新模型版本号 |
| `url` | 模型文件的相对路径（拼接到 `https://{PLATFORM_WS_HOST}` 前缀） |
| `md5` | 文件 MD5 校验值（小写十六进制） |
| `type` | 模型类型（写入 `model_type` 字段） |

### 设备 → 平台（进度反馈）

```json
{
  "action":  "UPDATE_COMMAND_PROGRESS",
  "status":  1,
  "version": "1.2"
}
```

| status 值 | 含义 |
|-----------|------|
| `0` | 开始下载 |
| `1` | 升级成功 |
| `-1` | 升级失败（MD5 校验失败、下载超时、配置文件找不到对应 channel 等） |

---

## 二次开发指南

### 修改 WebSocket 重连间隔

```python
await asyncio.sleep(5)  # 改为更长或更短的等待时间
```

### 支持多配置文件

若设备上有多个配置文件（如不同场景），可通过环境变量在启动时指定：

```bash
ASSETS_DIR=/path/to/assets CONFIG_FILE=config_scene1.json python3 ota_agent.py
```

在 `ota_agent.py` 中读取：

```python
CONFIG_PATH = os.path.join(ASSETS_DIR,
    os.environ.get("CONFIG_FILE", "config_mux16.json"))
```

### 添加下载进度回报

在 `execute_update()` 的下载循环中加入进度计算，并通过 `send_feedback_safe` 发送中间状态：

```python
total = int(r.headers.get('Content-Length', 0))
downloaded = 0
for chunk in r.iter_content(chunk_size=8192):
    f.write(chunk)
    downloaded += len(chunk)
    if total > 0:
        progress = int(downloaded / total * 100)
        # 可发送 status=progress（需平台侧支持中间进度消息）
```
