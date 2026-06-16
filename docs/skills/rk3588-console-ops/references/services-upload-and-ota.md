# 两个后台微服务（上报 / OTA）与各部分的关系

`service/` 下有两个 Python 微服务，**与通道逻辑松耦合、只通过文件和 Redis 交接**。写逻辑的人不用改它们，但要知道：你在逻辑里 `alarm_uploader_enqueue` / `dify_uploader_enqueue` 之后，数据就是被它们接走、转发出去的。

```
┌─ C++ 主程序 (rk3588_yolo) ───────────────────────────┐
│  通道逻辑 logic_xxx        （方案2：地址随记录/消息走）   │
│   ├ alarm_uploader_enqueue(...,server_url) ─┐ server: 写 .jpg+.json 落盘
│   └ dify_uploader_enqueue(...,api_url,key) ─┤ dify  : redis_rpush
└──────────────────────────────────────────────┘
       │ server 落盘                      │ dify RPUSH
       ↓                                  ↓
  本地发件箱 alarm_store/（磁盘）      Redis: dify_queue
       ↓ 扫目录                            ↓ BLPOP
┌─ 上报服务 unified_upload (main.py) ─────────────────────┐
│  OutboxForwarder → 扫发件箱读图 → POST 到 .json 里的 server_url（或 config.yaml 默认）→ 业务服务器
│                    成功即删；被拒/断网保留下轮重试（store-and-forward，断网不丢）
│  DifyUploader    → BLPOP dify_queue → 上传图+跑工作流 到消息里的 dify_api_url/key（或默认）→ Dify
└──────────────────────────────────────────────────────────┘

┌─ OTA 服务 ota_agent (ota_agent.py) ── 与上面无关，独立一条线 ─┐
│  WebSocket ⇄ 云平台  →  收升级指令  →  下载 .rknn → MD5 校验
│   → 放进 assets/ → 改写 config.json 的 model_path/version
│   → C++ config_monitor 检测到 → 热重载模型（无停机）
└──────────────────────────────────────────────────────────────┘
```

---

## 1. 上报服务 `unified_upload`（`service/upload/main.py`）

**职责**：板级一个的"转发器"。server 告警从**本地发件箱目录**取、Dify 从 **Redis 队列**取，转发到外部。

- **双线程**：`OutboxForwarder` 扫本地发件箱目录 `alarm_store/`（server 告警）、`DifyUploader`（经通用 `queue_worker`）`BLPOP` 消费 `dify_queue`（Dify 分析）。
- **OutboxForwarder**（server，store-and-forward）：扫 `alarm_store/` 里 C++ 落盘的 `.json`+`.jpg`，读图转 Base64 组业务 payload `POST`。地址 = 记录里的 `server_url` or config.yaml 默认。**传成功即删本地记录；服务器拒绝(非 200)/断网则保留、下轮重试**——断网期间告警攒在本地不丢，网络恢复后逐条补传。
- **DifyUploader**：先上传图片文件、再跑 Dify 工作流。地址/密钥 = `data.get("dify_api_url"/"dify_api_key") or 默认`。
- **配置 `config.yaml`**（同目录）：`server.url`/`dify.api_url`/`dify.api_key`/超时 = **默认/兜底地址**（通道留空时才用）；`redis.host/port/db/dify_queue` = 连接与 Dify 队列名（config.yaml 里的 `server_queue` 键已废弃不用——server 走本地发件箱）；发件箱目录由环境变量 `ALARM_STORE_DIR` 指定（两侧须一致）。
- **关键认知**：真正"发哪台服务器"由**记录自带的地址**决定（C++ 从 `ctx->config->server_url` 取、随告警写进发件箱 `.json`）。所以不同通道/程序发不同服务器，靠的是 config.json 里的每通道地址，不是这个服务；这个服务只是照着记录/消息发。

> 对应 C++：`src/uploader/alarm_uploader.cpp` 的 `record_alarm_local()`（server 落盘发件箱 `.json`，带 `server_url`）与 `redis_rpush("dify_queue", json)`（dify，带 `dify_api_url`/`dify_api_key`）。

## 2. OTA 升级服务 `ota_agent`（`service/model_update/ota_agent.py`）

**职责**：边缘端守护进程，与云平台保持 WebSocket 长连，收到升级指令就换模型，配合 C++ 热重载实现**无停机**模型升级。

- **连接**：`wss://{platform_ws_host}/ws/device/{DeviceID}`，DeviceID 由网卡 MAC→MD5 生成。
- **流程**：收 `UPDATE_COMMAND` → 比对版本（一致则跳过）→ 独立线程下载 `.rknn` 到 `/tmp` → **MD5 校验** → 移到 `assets/model_chN_<md5>.rknn`（旧模型保留）→ 改写**目标 config.json** 里该通道的 `model_path`/`version`/`model_type` → 回报进度。
- **配置 `ota_config.json`**（同目录）：`platform_ws_host`（平台地址）、`target_config`（要改的配置文件名，默认 `config.json`——必须和 C++ 实际在跑的那份一致）。`ASSETS_DIR` 由 systemd 单元的环境变量给。
- **与 C++ 的衔接**：它只改 `config.json`；C++ 的 `config_monitor` 线程检测到文件变化 → `algorithm_reload_channel_model` 热换模型（见 `adding-config-parameter.md` 的热重载机制）。**两者只通过 config.json 交接，不直接通信。**

---

## 3. 和各部分的关系（一张表）

| 它们与谁交接 | 怎么交接 |
|------------|---------|
| **C++ 主程序** | server 告警：经**本地发件箱目录** `alarm_store/`（C++ 落盘、Python 扫描补传）；Dify：经 **Redis 队列**（C++ 生产、Python 消费）。OTA：经**改写 config.json**（C++ 热重载）。都不直接调用。 |
| **Redis** | 仅 **Dify 路径**的消息总线（`dify_queue`）；server 告警不走 Redis（走本地发件箱）。OTA 不用 Redis。 |
| **config.json**（`assets/`） | 每通道**上报地址**在这（方案2，画布「上报配置」节点写）；OTA 升级会改这里的 `model_path/version`。 |
| **config.yaml**（源码 `service/upload/`，部署在 `<App>/services/upload/`） | 上报服务的**默认地址 + Redis + 超时**。 |
| **ota_config.json**（源码 `service/model_update/`，部署在 `<App>/services/model_update/`） | OTA 服务的平台地址 + 目标配置名。 |
| **网页控制台 · 「服务配置」弹窗** | 写 `config.yaml`（上报默认值）和 `ota_config.json`（OTA 参数）到**对应 App 的 services/ 目录**（后端 `routers/upload_config.py` / `ota_config.py`）。 |
| **网页控制台 · 「后台服务」面板** | 把这两个服务**安装成 systemd 单元并启停/看健康**（后端 `routers/services.py`）。 |
| **systemd** | 实际托管这两个 `.py` 的是 `ota_agent.service` / `unified_upload.service`。 |

## 4. 怎么启动、放在哪

- **代码位置**：随程序包打进每个 App：`/opt/ai_apps/<App>/services/model_update/`、`/opt/ai_apps/<App>/services/upload/`。
- **启动**：两条等价入口操作**同名 systemd 单元**——① 命令行 `deploy.sh` / `systemctl` / `python3 main.py`（开发调试）；② 网页「后台服务」面板 选 App 安装→启动。
- **板级一份**：两个服务整板各一个实例（共用 Redis、OTA 共用一个平台连接），单元 `WorkingDirectory` 绑定到**某一个 App** 的 services 目录。详见会话里"后台服务启动机制"的说明。

## 5. 对"写通道逻辑"的你意味着什么

- 你只需在逻辑里 `*_enqueue(...)` 并把地址参数从 `ctx->config->server_url`/`dify_api_url`/`dify_api_key` 取（用户在网页填）。**enqueue 之后的事这两个服务全包了**，你不用碰它们。
- 想验证有没有发出去：server 看本地发件箱 `ls <App>/alarm_store/`（Python 补传成功后即删）、dify 看 `redis-cli lrange dify_queue 0 -1`；上报服务日志看转发结果。
- 想改**分发规则**（比如按 `alarm_type` 改发往哪、加重试、加签名）：只动 `service/upload/main.py`，**不用动 C++、不用重编译**——这正是方案2 把"地址当数据、转发逻辑放 Python"的好处。
- OTA 跟通道逻辑基本无关，除非你的需求涉及"模型自动升级"，那也是平台侧推指令触发，逻辑层无感。

## 6. 二次开发：加第三个微服务
① `service/` 下加目录 + `.py` + 配置文件；② 若要 C++ 给它喂数据，参考 `alarm_uploader.cpp` 加一个 Redis 队列生产者；③ 控制台 `routers/services.py` 的 `MANAGED` 白名单加一项（unit 名/label/子目录 + 单元模板），前端「后台服务」面板就自动能装能管；④ 若要网页配它的参数，仿 `routers/upload_config.py` 加一个读写其配置文件的路由 + 在编辑器「服务配置」弹窗加一栏。

---

## 7. 网页 ↔ 板端：后台服务的启停怎么配合的

### 核心模型：systemd 是唯一管家，网页只是它的遥控器
板端真正托管这两个 `.py` 的是 **systemd**（`ota_agent.service` / `unified_upload.service`）。网页「后台服务」面板**不自己起进程**，而是**调 systemd**——后端 `web_console/backend/routers/services.py` 就是一层 `systemctl` 封装。控制台以 `User=root` 运行（`rk3588-console.service`），所以能直接 `systemctl` 和写 `/etc/systemd/system`。

> 对比：推理**二进制**(vision_app) 是控制台 `process_manager` 用 `subprocess.Popen` 直接管的；这两个 **python 服务**走 systemd。两套机制、各管一摊（维持分工，避免和 `vision_app.service` 双开）。

### 「安装」和「启动」到底在干嘛（关键心智模型，别被"安装"误导）

**「安装」不是装软件、不是装代码。** 服务代码 `ota_agent.py` / `main.py` **本来就在程序包里**（`/opt/ai_apps/<App>/services/...`，随包部署进来），不需要装。

分清三个角色就懂了：

| 角色 | 是什么 | 谁提供 |
|------|--------|--------|
| **代码** | `ota_agent.py`（真正干活的程序） | 程序包自带，在 `<App>/services/` |
| **单元文件** | `/etc/systemd/system/<svc>.service`（一张"怎么跑"的说明书：在哪跑/跑哪个文件/崩了拉起） | **「安装」就是生成/覆盖它** |
| **systemd** | 板上的进程管家（照说明书把代码跑起来、守着它） | 系统自带 |

所以：
- **「安装」= 写那张说明书**——单元里 `WorkingDirectory`/`ExecStart` 指向所选 App 的 `services/` 路径。
- **「重新安装 / 修复」= 把说明书里的路径改对**（强制覆盖重写，指向当前程序包）。
- **「启动」= `systemctl start`**——前提是说明书已存在且路径正确（systemd 照它 `cd WorkingDirectory` → 跑 `python3 …`；这一步切不进目录就是 `CHDIR No such file or directory`）。

**为什么分两步**：没有说明书 / 说明书路径失效 → systemd 不知道怎么跑，必须先「安装/修复」把说明书写对；说明书已就位 → 以后只用「启动/停止」开关进程，不再写说明书。面板的判定就是：`path_ok=false`（说明书指向的目录不存在）→ 逼你走「🔧 修复并启动」；`path_ok=true` → 给「启动/停止」开关。

```
点「安装 / 修复并启动」
  └ 写 <svc>.service（说明书，路径指向当前 App）→ daemon-reload(重读) → enable(自启)
      → reset-failed(清旧失败) → restart(启动)
         └ systemd: cd WorkingDirectory → exec python3 ota_agent.py → 守护(崩了 Restart=always 拉回)

点「启动 / 停止」（仅当说明书已就位且路径对）
  └ systemctl start / stop <svc>
```

> 典型坑：报 `CHDIR ... No such file or directory` = 说明书还是旧的、指向已删目录（如换过项目目录后旧的 `/userdata/.../dist/...`）。「重新安装」把说明书重写对即可，**服务代码一直在程序包里没动过**。

### 网页按钮 ↔ 后端动作 ↔ 板端命令
| 面板操作 | 后端 `routers/services.py` | 板端实际执行 |
|---------|---------------------------|------------|
| 选 App「安装 / 修复并启动」 | `POST /api/services/{key}/install` | **强制覆盖**写 `/etc/systemd/system/<unit>`（`WorkingDirectory`/`ExecStart` 指向所选 App 的 `services/`；任何旧路径/失效单元直接被改掉）→ `daemon-reload` → `enable` → `reset-failed` → **`restart`（装完即启动）** |
| 「启动」/「停止」 | `POST /api/services/{key}/start`/`stop` | 启动前先校验 `WorkingDirectory` 存在（不存在 → 明确报错而非底层 CHDIR）→ `reset-failed` → `systemctl start`/`stop <unit>` |
| 状态刷新（每 5s 轮询） | `GET /api/services` | `systemctl show <unit> --property=…,WorkingDirectory`；据此算 `path_ok`（工作目录是否真实存在）与 `bound_app` |
| 「日志」 | `GET /api/services/{key}/logs` | `journalctl -u <unit> -n N --no-pager` |

> **安全**：用户只传白名单 key（`ota_agent`/`unified_upload`），unit 名是服务端常量，绝不把任意串拼进 `systemctl`。
>
> **失效单元自动识别 + 强制修正**：状态里的 `path_ok` 标记单元 `WorkingDirectory` 是否真实存在。指向已删目录的残留旧单元（如旧的 `/userdata/.../dist/...`、换过项目目录后）→ `path_ok=false`，面板把它标成「⚠ 路径失效」、按钮换成「🔧 修复并启动」。选当前 App 点一下，后端**强制把单元重写到 `/opt/ai_apps/<App>/services/...` 并启动**——服务永远跟着程序包走，不用手动 `rm` 旧单元。

### 为什么网页和命令行不会打架
两边**操作的是同一套同名 systemd 单元**——网页 `systemctl start ota_agent` 和你在板上敲 `systemctl start ota_agent` 是同一个对象。所以：命令行停了它，网页状态 5s 内变「已停止」；网页装的单元，命令行 `systemctl status` 照样看得到。systemd 是唯一事实来源，网页只读它的状态、发它能懂的命令——不存在"网页一份、命令行一份"的分叉。

### 状态/按钮 ↔ systemd 状态的映射（前端 `ServicesPanel`）
- 状态徽章由 `ActiveState`/`LoadState` 推出：`active`→🟢运行中、`activating`→…启动中、`failed`→🔴故障、`inactive`→⚪已停止、`LoadState!=loaded`→🟡未安装；`NRestarts`→"尝试重启 N 次"（systemd 因 `Restart=always` 拉起过几次）。
- **按钮是单一开关**：只要服务"开着"就显示「停止」——判定 = `ActiveState` 不是 `inactive` 也不是 `unknown`（即 `active`/`activating`/`failed` 都算开）。这样**服务一直起不来、在那反复重启时按钮仍是「停止」**，你能摁停下来排查（`systemctl stop` 会把 `Restart=always` 的重启循环和 failed 状态一并停掉）。只有干净 `inactive`/未装才显示「启动」。

### 与"绑定哪个 App"的关系
`install` 时单元里写死了 `WorkingDirectory=/opt/ai_apps/<所选App>/services/...`，所以**服务跑的是那个 App 的代码 + 读那个 App 的 config.yaml/ota_config.json**。面板每行显示的 `bound_app` 就是从单元的 `FragmentPath`→`WorkingDirectory` 反解出来的，用来核对"配置的 App == 绑定的 App == 在跑的 App"（三者要一致，否则配的不是跑的那份）。

### 在板端直接启停（不经网页，开发者常用）

因为网页和命令行操作的是**同一套 systemd 单元**，在板上直接来效果完全一样。

**A. 单元已装**（`deploy.sh` 装过、或网页面板「安装」过；`systemctl cat ota_agent` 能看到内容即已装）：
```bash
sudo systemctl start   ota_agent unified_upload     # 启动
sudo systemctl stop    ota_agent unified_upload     # 停止
sudo systemctl restart ota_agent                    # 重启
sudo systemctl status  ota_agent --no-pager         # 看状态
sudo systemctl reset-failed ota_agent && sudo systemctl start ota_agent   # 反复起不来时先清 failed 再起
journalctl -u ota_agent -f                          # 实时日志（两个服务的 print 都进 journal）
journalctl -u unified_upload -n 50 --no-pager       # 看最近 50 行
sudo systemctl enable  ota_agent                    # 开机自启 / disable 取消
```

**B. 裸跑调试**（直接前台跑 `.py`，Ctrl+C 退出——排"为什么起不来"最快，终端直接看到所有报错）：
```bash
# OTA：必须给 ASSETS_DIR 指向该 App 的 assets/（systemd 单元里就是靠 Environment 设的）
cd /opt/ai_apps/<App>/services/model_update
ASSETS_DIR=/opt/ai_apps/<App>/assets python3 -u ota_agent.py

# 上报服务：读同目录 config.yaml，无需额外环境变量
cd /opt/ai_apps/<App>/services/upload
python3 -u main.py
```

**C. 一键装单元 + 启动**（build.sh 产物自带，开发者标准流程）：
```bash
cd <dist目录>                        # build.sh 产物（含 rk3588_yolo / services/ / deploy.sh）
bash deploy.sh ./assets/config.json  # 交互式生成并注册 vision_app/ota_agent/unified_upload，按提示选启动
```

**D. 手动写一个 OTA 单元**（没有 deploy.sh、想自己装时；上报服务同理：去掉 `Environment`、`ExecStart` 改 `WorkingDirectory` 下的 `main.py`）：
```ini
# /etc/systemd/system/ota_agent.service
[Unit]
Description=Edge Box OTA Agent
After=network.target
[Service]
Type=simple
WorkingDirectory=/opt/ai_apps/<App>/services/model_update
Environment=ASSETS_DIR=/opt/ai_apps/<App>/assets
ExecStart=/usr/bin/python3 -u /opt/ai_apps/<App>/services/model_update/ota_agent.py
Restart=always
RestartSec=3
User=root
[Install]
WantedBy=multi-user.target
```
```bash
sudo systemctl daemon-reload && sudo systemctl enable --now ota_agent
```
> ⚠ `WorkingDirectory` 必须是**真实存在**的目录，否则启动即报 `CHDIR ... No such file or directory`（systemd 切不进去就不会起进程）。残留的旧单元若指向已删除的路径（如旧的 `/userdata/dist/...`），网页/命令行启动都会这样失败——重装单元让它指向当前真实路径即可。

> 一句话：**网页对后台服务的所有操作 = 翻译成 `systemctl`/`journalctl` 打给板端 systemd；systemd 才是真管家，网页是带状态灯的遥控器。** 这也是它能和命令行无缝并存的原因。
