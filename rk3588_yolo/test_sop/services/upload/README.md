# 告警上传微服务 (services/upload/)

> Python 异步消费服务，从 Redis 队列读取告警数据，分别上报到业务服务器和 Dify AI 工作流。

---

## 文件清单

| 文件 | 职责 |
|------|------|
| `main.py` | 服务主入口：创建两个 worker 线程分别消费 `server_queue` 和 `dify_queue` |
| `config.yaml` | 服务配置：Redis 连接、业务服务器地址、Dify API 密钥等 |
| `requirements.txt` | Python 依赖 |

---

## 架构设计

```
Redis
  ├── server_queue  ← C++ 主程序 alarm_uploader_enqueue() 写入
  └── dify_queue    ← C++ 主程序 dify_uploader_enqueue() 写入

main.py
  ├── ServerWorker 线程
  │     └── BLPOP server_queue → ServerUploader.upload()
  │              └── HTTP POST → 业务服务器
  └── DifyWorker 线程
        └── BLPOP dify_queue   → DifyUploader.upload()
                 ├── POST /v1/files/upload → 获取 file_id
                 ├── POST /v1/workflows/run → 触发工作流
                 └── 删除本地临时图片文件
```

两个 worker 线程独立运行，互不干扰。Redis `BLPOP` 为阻塞拉取，队列为空时线程休眠，无 CPU 空转。

---

## 配置说明（config.yaml）

```yaml
# Dify AI 工作流配置
dify:
  api_url: "http://192.168.2.98:8015"   # Dify 服务地址（支持只填 host:port）
  api_key: "app-xxxxxxxx"               # Dify 应用 API Key
  timeout: 30                           # HTTP 超时（秒）

# 业务服务器配置
server:
  url: "http://192.168.2.22:9200/api/objectInvadeDet"  # 接收告警的 HTTP 接口
  timeout: 15                           # HTTP 超时（秒）

# Redis 配置
redis:
  host: "localhost"
  port: 6379
  db: 0
  server_queue: "server_queue"          # 业务服务器队列名
  dify_queue: "dify_queue"              # Dify 队列名
```

---

## 部署与运行

### 安装依赖

```bash
cd dist/services/upload
pip3 install -r requirements.txt
```

### 启动服务

```bash
python3 main.py
```

建议配合 `systemd` 或 `supervisor` 实现开机自启和崩溃自动重启。

### systemd 配置示例

```ini
[Unit]
Description=RK3588 Alarm Upload Service
After=redis.service

[Service]
ExecStart=/usr/bin/python3 /path/to/dist/services/upload/main.py
WorkingDirectory=/path/to/dist/services/upload
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

---

## 业务服务器接口规范

`ServerUploader` 向业务服务器发送的 POST 请求体格式：

```json
{
  "source":        "JNU",
  "eventType":     "4005",
  "detResult":     {},
  "snapTime":      "2026-05-08 12:00:00",
  "endTime":       "2026-05-08 12:00:00",
  "base64Data":    "<JPEG Base64，带标注框>",
  "base64DataRaw": "<JPEG Base64，原始帧>",
  "invadeFlag":    1
}
```

业务服务器返回 HTTP 200 视为成功，其他状态码打印错误日志后丢弃该条消息（不重试）。

---

## 二次开发指南

### 修改上报目标服务器

**方案2（每通道独立地址）**：上报地址随 Redis 消息下发——优先用消息自带的 `server_url`（HTTP）/ `dify_api_url`+`dify_api_key`（Dify），**留空才回落**到 `config.yaml` 的默认值。

- 改**单个通道**的地址：在网页编辑器「上报配置」节点里填该通道的地址（写进 `config.json`，由 C++ 随消息下发），无需动本服务。
- 改**全局默认**地址：编辑本目录 `config.yaml` 的 `server.url` / `dify.api_url`，重启服务生效。
- 调试某条告警发去哪：`redis-cli lrange server_queue 0 -1` 直接能看到消息里的 `server_url`。

### 修改 Dify 工作流

`DifyUploader` 支持在 Redis 消息中携带自定义工作流输入参数：

```json
{
  "image_path": "/tmp/dify_ch0_xxx.jpg",
  "prompt": "检测画面中是否有安全帽",
  "event_id": "ch0_f1234",
  "inputs": {
    "custom_param": "value"
  }
}
```

`inputs` 字典会被合并到 Dify `workflows/run` 请求的 `inputs` 字段中，`image_var_name`（默认 `"image"`）指定图片在工作流中对应的变量名。

### 添加新的上报目标

在 `main.py` 中新增一个 `XxxUploader` 类，实现 `upload(data: dict) -> bool` 方法，然后在 `main()` 中新增 worker 线程：

```python
class MyUploader:
    def upload(self, data):
        # 实现你的上报逻辑
        return True

my_uploader = MyUploader()
t3 = threading.Thread(
    target=queue_worker,
    args=(config, "my_queue", my_uploader),
    name="MyWorker",
    daemon=True,
)
t3.start()
```

同时在 C++ 侧 `alarm_uploader.cpp` 中新增对应的 `redis_lpush("my_queue", ...)` 调用。

### Redis 连接断线重连

worker 线程捕获 `redis.exceptions.ConnectionError` 后等待 2 秒自动重连，无需手动干预。
