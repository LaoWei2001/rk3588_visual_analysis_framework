# 上报链路单元测试（producer 端）

这两个测试**直接引用主程序源码** `src/uploader/alarm_uploader.cpp` 的真实函数
（`alarm_uploader_init` / `alarm_uploader_enqueue` / `dify_uploader_enqueue` / `alarm_uploader_deinit`），
模拟 C++ 视觉程序「把一条报警/分析任务投递进 Redis 队列」的真实行为，用来验证两条上报链路的**生产者**一端：

| 测试 | 验证的链路 | 落点 |
|---|---|---|
| `test_server/` | `alarm_uploader_enqueue()` → C++ 落盘本地发件箱 `alarm_store/`（带框图 + 原图 + .json，**不经 Redis**）→ Python `OutboxForwarder` 扫描补传 → HTTP POST 业务服务器 | 本地 `alarm_store/` 目录 |
| `test_dify/`   | `dify_uploader_enqueue()` → Redis `dify_queue` → Python `DifyUploader` → 触发 Dify 工作流 | Redis `dify_queue` |

> 这里只测 producer 一端（C++ 是否把数据正确投出去）：**server 落盘到本地发件箱、dify 进 Redis 队列**——两条路落点不同。下游 Python 微服务的消费（HTTP / Dify）由 `service/upload` 负责，验证时可另起该服务观察它把发件箱/队列清掉。

## 前置条件

- 在 **RK3588 板子（aarch64）** 上构建运行（与主程序同环境）。
- 已装编译依赖：`./install_deps.sh --build`（含 OpenCV / hiredis 等 `-dev` 包）。
- 本机 **Redis 已启动**：`redis-cli ping` 应返回 `PONG`。

## 编译

```bash
cd rk3588_yolo/tests/test_server && bash build.sh     # 产出 ./server_queue_producer_test
cd ../test_dify                  && bash build.sh     # 产出 ./dify_queue_producer_test
```

`build.sh --clean` 可清掉 `build/` 重新编译。两个产物都是构建产物（已在 `.gitignore`，不入库）。

## 运行

```bash
# server：默认自动定位仓库内 service/upload/config.yaml，自动生成测试图
./test_server/server_queue_producer_test
#   可选：--image-draw <带框图> --image-raw <原图> --config <自定义config.yaml>

# dify：必须给一张图片，提示词从 test_dify/prompt.yaml 读
./test_dify/dify_queue_producer_test test_dify/hmd.png
#   可选：--config <自定义config.yaml>
```

两个测试都通过编译期宏 `SOURCE_ROOT` 自动定位仓库根目录下的 `service/upload/config.yaml`
（取 `redis.host` / `redis.port`），无需手动 `--config`；部署在没有源码树的板子上时再用 `--config` 指定。

## 验证结果

测试进程打印 `enqueue success=N/N` 表示已成功投递。再看数据真的落到位了没有：

```bash
# server：落盘到运行目录下的 alarm_store/（每条 = 带框图.jpg + 原图_raw.jpg + .json）
ls -l ./alarm_store/

# dify：进 Redis 队列
redis-cli llen   dify_queue
redis-cli lrange dify_queue 0 -1     # 看具体 JSON（base64 图较长）
```

> server 发件箱默认是**运行测试时所在目录**下的 `./alarm_store/`，可用环境变量 `ALARM_STORE_DIR` 覆盖。

要端到端验证「投递 → 被消费 → 真正发出去」，可同时启动上报微服务
（见 `docs/skills/rk3588-console-ops/references/services-upload-and-ota.md`），
它会扫描发件箱补传 / 消费 Redis 队列并执行 HTTP / Dify 调用。

## 端到端联调（producer + consumer，逻辑同正式部署）

`test_server/outbox_consumer_test.py` 是本地发件箱的消费端，**直接复用正式部署 `service/upload`
里的 `OutboxForwarder`**（import 进来用、不复制逻辑）。不带参的持续模式直接调用线上 `run()`，
与线上逐字一致（重试/退避、送达即删、断网攒着、payload 格式）；`--once` 复用线上的逐条转发
`_forward_one()`、跑一轮即退（不含多轮退避）。配合 producer 就是一条完整链路：

```bash
cd rk3588_yolo/tests/test_server
bash build.sh

# ① 先在 service/upload/config.yaml 把 server.url 改成你的服务器地址（不改就连默认地址）
# ② producer：落盘到同级 ./alarm_store/（每条 = 带框图.jpg + 原图_raw.jpg + .json）
./server_queue_producer_test

# ③ consumer：扫描同级 ./alarm_store/ → POST → 传成功即删
python3 outbox_consumer_test.py --once     # 补传一轮后退出（快速验证）
# 或 python3 outbox_consumer_test.py        # 持续运行，同正式部署，Ctrl+C 退出
```

producer 默认落盘到运行目录下的 `./alarm_store/`，consumer 默认读**自己同级**的 `alarm_store/`——
都在 `test_server/` 下运行时天然对齐；换目录时给两边同样的 `--store` / `ALARM_STORE_DIR` 即可。

- 服务器**通了**：consumer 打印 `sent`，对应记录从 `alarm_store/` 删除。
- 服务器**不通**：打印 `down`，数据留在本地——这正是 "断网攒着、重连补发" 的有效验证。

**Dify 路径**（Redis 队列，位置无关——任何能连同一个 Redis 的地方都能消费）：

```bash
cd rk3588_yolo/tests/test_dify
bash build.sh

# ① 先在 service/upload/config.yaml 配好 dify.api_url / dify.api_key（或让消息自带）
# ② producer：把带图消息 RPUSH 到 Redis dify_queue
./dify_queue_producer_test hmd.png

# ③ consumer：BLPOP dify_queue → 上传图片 + 触发 Dify 工作流
python3 dify_queue_consumer_test.py --once     # 消费完当前积压后退出
# 或 python3 dify_queue_consumer_test.py        # 持续运行，同正式部署 queue_worker，Ctrl+C 退出
```

与 server 不同，dify 走 Redis：consumer 不必和 producer 同机，只要连到 C++ 推送的那台 Redis
（同 host:port、**同 db 0**、队列名 `dify_queue`）即可。`dify_queue_consumer_test.py` 直接复用
`service/upload` 的 `DifyUploader` / `queue_worker`：不带参直接调用线上 `queue_worker()`（逐字一致），`--once` 复用 `DifyUploader.upload()` 跑一轮即退。要看到 `ok` 需有真实可用的 Dify 端点。

## 注意

- 投递函数当前是 **C 风格签名**（`const char *`，见 `src/uploader/alarm_uploader.h`）；
  测试里凡是 `std::string` 都要 `.c_str()` 后再传。改 API 时记得同步这两个测试。
- server 落盘 `.json` 的字段格式由 `alarm_uploader.cpp` 决定（`img` / `img_raw` 文件名、`server_url`、`snapTime` 等）；
  带框图 / 原图随后由 Python(`OutboxForwarder`) 读成 `base64Data` / `base64DataRaw` 再 POST，下游解析需与之一致。
- **复用边界**：消费端不带参的持续模式 = 直接调用线上 `run()` / `queue_worker()`（逐字一致）；
  `--once` 复用线上的逐条转发（`_forward_one` / `DifyUploader.upload`），只是外层循环是测试代码（快速验证用）。
- **上报地址**：producer 不在消息里带地址，消费端因此用 `service/upload/config.yaml` 的 `server.url` / `dify.api_url`——
  即测试用的就是正式 `config.yaml` 里的地址（与「按默认地址部署、通道留空」的实际用法一致，这是预期行为）。
  （方案2 的「逐通道地址随消息下发」是另一条分支，本测试不涉及；真要单独测它，给 producer 传个地址即可。）
