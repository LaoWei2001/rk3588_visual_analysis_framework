---
name: rk3588-channel-logic
description: >-
  Implements a new "channel logic" (a logic_xxx function) for the RK3588 YOLO
  vision system from a natural-language detection/alarm requirement. Use this
  skill whenever a developer describes a per-channel detection, tracking, dwell,
  intrusion, counting, or alarm rule for this project and wants it built — e.g.
  "区域内有人停留3秒就报警并上报服务器", "有车进入禁区就发 Dify 核验", "数一下画面里
  戴安全帽的人", "when a person leaves the ROI for 5s, upload to the server".
  Trigger even if they don't say the word "logic" — if it's a "当检测到 X 就做 Y"
  rule for a channel in this RK3588 project, use this skill. Also trigger on any
  edit to channel_logic.cpp / ChannelContext / logic_server / logic_dify, or when
  wiring, or removing/deleting, a logic in config.json / logics.json (e.g. "删除/移除
  某个通道逻辑 logic_xxx"). Do NOT use for the web console,
  the Python upload/OTA services, or model/config changes unrelated to logic.
---

# RK3588 通道逻辑（channel logic）开发

把开发者一句话的检测/报警需求，落成项目里一个能直接编译运行的 `logic_xxx` 通道逻辑，并接好线（能够对接前端web配置界面以及后端用于发送报警信息等功能的微服务）。开发者只需验证。

## 你的产出（output）

针对一个需求，你要给出**一组改动**，全部对齐项目现有约定：

1. **`rk3588_yolo/src/logic/logic_xxx.cpp`（新建独立文件，一个逻辑一个文件）** — 顶部 `#include "logic_common.h"`，实现 `static void logic_xxx(ChannelContext *ctx)`，文件**末尾写一行** `REGISTER_LOGIC("logic_xxx", logic_xxx);` 自注册（在 `main()` 前自动登记，**无需改动 `channel_logic.cpp`**）。`src/logic` 下的 `.cpp` 由 CMake（`aux_source_directory`）自动收集编译。`channel_logic.cpp` 现在只是框架核心（`ChannelContext` 方法 / `draw_*` / 分发表），不再放具体逻辑。
2. **`rk3588_yolo/src/logic/logics.json`** — `channel_logics` 数组加一条声明（`name`/`label`/可选 `report`/`params`），让网页编辑器能选到它、渲染可调参数。
3. **（仅当需要新的可调参数时）** `rk3588_yolo/src/config/config.h` 的 `ChannelConfig` 加字段 + `rk3588_yolo/src/config/config_init.cpp` 用 `REG_C` 注册。
4. **收尾说明**给开发者：哪个通道的 `config.json` 里 `"logic"` 要设成 `"logic_xxx"`（或叫他在网页画布的逻辑节点选这个 logic）、要不要连「上报配置」节点、改完怎么编译验证。

> 不要直接改 `config.json`（那是每台设备/每个 App 的运行配置，由网页编辑器管）。你只改源码 + logics.json + 给接线说明。

## 核心心智模型（先理解这些，代码自然就对）

- **逻辑是"每通道、每帧"被调用一次的纯函数**：`logic_xxx(ctx)`。同一个函数被多个通道复用，靠 `ctx` 区分——`ctx->config` / `ctx->results` / `ctx->roi` / `ctx->chnId` 都是**本通道、本帧**的数据。函数本身不存状态。
- **坐标系统一是"模型输入尺寸"（通常 640×640）**：`ctx->results[].box` 和 `ctx->roi` 都在这个坐标系，画图也用这个坐标系。**不要**用源视频分辨率去算。这点是项目大量坐标 bug （如实际视频窗口中显示的ROI区域偏离预先绘制的ROI区域）的根源，务必警惕。
- **跨帧记忆（计时、去重、闩锁）放在 `ctx->state`**：每通道独立的一格 `shared_ptr`。模式见下。
- **上报地址是"每通道"的，跟着告警走（方案2）**：你不在逻辑里硬编码服务器地址。需要上报时调 `alarm_uploader_enqueue(...)` / `dify_uploader_enqueue(...)`，地址参数从 `ctx->config->server_url` / `dify_api_url` / `dify_api_key` 取（用户在网页「上报配置」节点逐通道填）。详见 `references/upload-and-wiring.md`。
  - 想深入了解 `*_enqueue` 之后数据去哪（两个微服务的转发机制、OTA 换模型、与 config.yaml/网页/systemd 的关系），见独立的 **`rk3588-console-ops`** skill（控制台/部署/运维）。本 skill 的 `upload-and-wiring.md` §1 已有够用的下游概述，写逻辑本身不必跳过去。

## 从需求到代码：先把需求拆成这几维

读懂需求后，在心里（或跟开发者确认）填这张表，再动手写：

| 维度   | 要确定的                       | 常见取值                                                                            |
| ---- | -------------------------- | ------------------------------------------------------------------------------- |
| 目标类别 | 检测哪个/哪些 label              | `"person"` / `"car"` / 自定义模型的类名（**大小写/拼写要和 labels.txt 完全一致**）                   |
| 区域   | 全屏还是 ROI 内                 | 用 `roi_has_target(ctx, label, ROI_ALL)` 或自己 `pointPolygonTest(*ctx->roi, box_center)` |
| 触发条件 | 存在 / 计数 / 停留时长 / 越界 / 位置关系 | 停留/越界要用 `ctx->state` 跨帧计时                                                       |
| 动作   | 画框/文字、报警、上报(server/dify)   | 上报见 `references/upload-and-wiring.md`                                           |
| 频率   | 上报/报警限频                    | 用 `ctx->timestamp_ms` 做间隔，别每帧都发，必须要设定冷却时间或者其他能够限制连续触发报警的规则。                     |

如果需求里这些维度有歧义（比如"停留多久""发到哪种上报"），**简短问一句**再写，别瞎猜。

## 写逻辑的骨架（作为参考）

```cpp
// 跨帧状态：本通道独有，跨帧保留（计时/闩锁/去重都放这）
struct XxxState {
    uint64_t last_upload_ms = 0;
    bool     alarm_latched  = false;
};

static void logic_xxx(ChannelContext *ctx)
{
    if (!ctx || !ctx->results) return;

    // 1) 取/建本通道跨帧状态
    if (!*ctx->state) *ctx->state = std::make_shared<XxxState>();
    auto &s = *std::static_pointer_cast<XxxState>(*ctx->state);

    // 2) 遍历本帧检测结果，按需求判定（坐标都在模型 640 空间）
    int has_roi = (ctx->roi && ctx->roi->size() >= 3);
    bool hit = false;
    for (auto &r : *ctx->results) {
        if (r.label != "person") continue;                       // 类别过滤
        if (has_roi && cv::pointPolygonTest(*ctx->roi, r.box_center(), false) < 0)
            continue;                                            // ROI 过滤（框中心在区域内）
        hit = true;
        r.box_color = cv::Scalar(0, 0, 255);                     // 命中的框标红（可选）
    }

    // 3) 状态机 / 计时（示例：满足条件并限频后上报）
    if (hit) {
        if (ctx->timestamp_ms - s.last_upload_ms > 5000 && ctx->frame) {
            const char *url = (ctx->config && !ctx->config->server_url.empty())
                                  ? ctx->config->server_url.c_str() : nullptr;
            alarm_uploader_enqueue(*ctx->frame, *ctx->frame, ctx->chnId, "xxx_alarm", url);
            s.last_upload_ms = ctx->timestamp_ms;
        }
        draw_text(ctx, "ALARM", cv::Point(20, 30), cv::Scalar(0,0,255), 0.7, 2);
    } else {
        draw_text(ctx, "CLEAR", cv::Point(20, 30), cv::Scalar(0,255,0), 0.7, 2);
    }
}
```

`ctx` 的全部字段与可用辅助函数（`roi_has_target`、`draw_*`、`get_channel_snapshot` 等）见 **`references/channelcontext-api.md`**。

想了解你的逻辑跑在什么样的运行时里（8 类线程、同步原语、**帧与检测框为何保证同帧**、启动/退出时序，以及上报地址/ROI 坐标系等架构要点），看 **`references/rk3588_yolo_系统说明文档.md`**（文字详解）+ **`references/rk3588_yolo_架构图.md`**（图）。写普通逻辑不必读；但要碰 analyzer/capturer/uploader 核心、或排查并发/时序/坐标问题时，先读它们建立全局观。

## 现有逻辑参考（`references/examples/`，一个文件一个真实函数）

项目里已有的每个 `logic_xxx` 都拆成了独立 md（含完整真实代码 + 接线 + 复用提示）。**写新逻辑前，可先按需求挑一个最接近的去读、照它的范式改**——这些是项目里跑通的代码，照搬模式最稳：

| 需求里出现                      | 参考这个现有逻辑                                                | 文件                                                          |
| -------------------------- | ------------------------------------------------------- | ----------------------------------------------------------- |
| 有/出现 X 就上报服务器              | `logic_server`（最简骨架）/ `logic_person_alarm`（带判定+可视化，未上报） | `examples/logic_server.md`、`examples/logic_person_alarm.md` |
| 持续 N 秒才告警、只发一次、条件消失后复位     | `logic_hook`（闩锁 + 双向计时 + 丢检宽限）                          | `examples/logic_hook.md`                                    |
| 每隔 N 秒发一次 AI 核验            | `logic_dify`                                            | `examples/logic_dify.md`                                    |
| 每个**新**目标只报一次、去重防刷         | `logic_dify_person_verify`（按 track_id 去重 + 丢帧容忍）        | `examples/logic_dify_person_verify.md`                      |
| 区域内计数 / 逐目标染色标注            | `logic_custom`                                          | `examples/logic_custom.md`                                  |
| 跨摄像头 / 多路联动                | `logic_cross_camera`（跨通道原子快照）                           | `examples/logic_cross_camera.md`                            |
| YOLO 检不出、要用传统视觉（占用率/颜色/帧差） | `logic_roll`（ROI 内 HSV 占用率）                             | `examples/logic_roll.md`                                    |
| 目标轨迹 / 扫过面积 / ROI 覆盖率（巡检/擦拭/喷涂到位） | `logic_wafer`（轨迹拓宽成带 + 掩码求覆盖率 + 消失复位）              | `examples/logic_wafer.md`                                   |
| 轨迹自动分段/动作判别（横擦/环擦）+ 椭圆 ROI 提取 + 中文图例 | `logic_wafer2`（visualize_trajectories.py 实时移植）          | `examples/logic_wafer2.md`                                  |
| ROI 顶点/坐标可视化、按区域染框 | `logic_roi`（单 ROI）/ `logic_multi_roi`（多命名区域 `roi_by_name`） | `examples/logic_roi.md`、`examples/logic_multi_roi.md` |
| 按顺序经过多个命名工位/区域的 SOP（顺序/朝向/停留逐步点亮） | `logic_wafer_sop`（命名多 ROI + 进入/朝向双防抖 + 往复计数） | `examples/logic_wafer_sop.md` |
| 跌倒 / 姿态 / 挥手求救（pose 关键点，dwell + 冷却上报） | `logic_fall_detect`（pose 关键点 + 框比例退回 + dwell 限频） | `examples/logic_fall_detect.md` |
| 占位 / 临时关掉某通道业务             | `logic_default`                                         | `examples/logic_default.md`                                 |

> 新增了逻辑后，也照这个格式在 `examples/` 里补一个 `logic_新名.md`，让这套参考库一直跟着代码长。

## 接线三件套（写完逻辑必做）

1. **注册**（在该 logic 自己的 `src/logic/logic_xxx.cpp` 文件**末尾**自注册，无需改动 `channel_logic.cpp`）：
   
   ```cpp
   REGISTER_LOGIC("logic_xxx", logic_xxx);
   ```
2. **声明到 logics.json**（让网页能选、能渲染参数；`report` 决定要不要连「上报配置」节点）：
   
   ```json
   { "name": "logic_xxx", "label": "中文名", "report": "server", "params": [] }
   ```
   
   - `report`: 用了 `alarm_uploader_enqueue` 填 `"server"`；用了 `dify_uploader_enqueue` 填 `"dify"`；不上报就不写这个键。
3. **可调参数**（仅当逻辑需要用户在网页改的数值，如"半径""停留秒数"）必须走以下步骤，缺一不可：
   
   - `ChannelConfig`（config.h）加字段，如 `int dwell_sec = 3;`
   
   - `config_init.cpp` 加 `REG_C("dwell_sec", INT, dwell_sec);`
   
   - logics.json 该 logic 的 `params` 加一条 `{ "key": "dwell_sec", "type": "int", "label": "停留秒数", "default": 3, "min": 1, "max": 60 }`
   
   - 逻辑里读 `ctx->config->dwell_sec`（**每帧现读，别缓存**）。
   **完整链路（加字段 + 热重载 + 网页可配的机制、类型表、例外、坑）详见 `references/adding-config-parameter.md`**。关键点：REG_C 注册的参数**自动参与热重载**——用户网页改值保存后，`config_monitor` 线程经 `sync_fields` 热拷进运行配置，逻辑下一帧即读到新值，**无需重启**（前提是逻辑里每帧从 `ctx->config` 现读、不缓存）。

## 删除一个通道逻辑（反向拆"接线三件套"）

要删掉一个已有的 `logic_foo`，把它的注册、定义、引用反向拆干净：

**必做（否则编译失败或残留）**

1. **删除整个文件 `src/logic/logic_foo.cpp`** —— 该逻辑的函数定义、它**专用**的辅助结构/函数（`struct FooState`、专用 helper）、以及末尾的 `REGISTER_LOGIC("logic_foo", logic_foo);` 都自包含在这一个文件里，直接删掉即可，不牵连任何其它文件（CMake 重新 configure 后自动从编译列表移除）。⚠ 若该逻辑用到的 State 结构定义在 **共享的 `logic_tools.h`**（如 `HookState`/`PersonAlarmState`/`DifyPersonVerifyState`），那是多处共用，删文件时别动它（除非确认无人再用）。
2. **`logics.json`** —— 删 `channel_logics` 数组里那条 `{ "name": "logic_foo", ... }`（保持 JSON 合法，注意逗号）。

**按情况清理**

3. **可调参数**：若它当初在 `config.h` / `config_init.cpp` 加过**专属**字段，删字段 + 对应 `REG_C`。⚠ **共享字段别删**（如 `radius` 被 `logic_hook`/`logic_roll` 共用，删了会连累别人）。
4. **其它引用**：全局逻辑里的 `channel_has_logic(ch, "logic_foo")` / `static_pointer_cast<FooState>`；`logic_tools.h` 里它的 State 结构（若无人再用）；前端硬编码上报名单 `SERVER_LOGICS`/`DIFY_LOGICS`（仅当当初加过——正常用 logics.json 的 `report` 声明则这里没有它）。
5. **运行配置 `config.json`**：把 `"logic": "logic_foo"` 的通道改成别的逻辑（到网页画布重选「逻辑函数」节点）。不改不会崩——运行时 `channel_logic_get` 返回 null、`invoke_channel_logic` 里 `if (!fn) return;` 静默跳过，但该通道**不跑任何逻辑**，且网页逻辑下拉会把它显示成"未知项"。
6. **文档收尾**：删 `references/examples/logic_foo.md`，并去掉本文件"现有逻辑参考"表里那一行，保持文档与代码同步。

**找全引用 + 生效**

```bash
grep -rn "logic_foo\|FooState" rk3588_yolo/src web_console/frontend/src
```

改完编译装包 `cd rk3588_yolo && ./build.sh <名> && sudo ./install_app.sh <名>` → 网页「程序管理」重启该程序；若动了前端名单，再 `cd web_console && bash install.sh` + 浏览器硬刷新。

> 最小集 = ①(删函数+注册) + ②(删 logics.json 条目) + ⑤(重指通道)；③④⑥ 视有无牵连而定。

## 给开发者的收尾说明（每次都要附上）

写完后，明确告诉开发者**验证清单**：

1. 在哪个通道生效：网页画布该通道的「逻辑函数」节点选 `logic_xxx`（或手改 `config.json` 该通道 `"logic": "logic_xxx"`）；若上报，连一个「上报配置」节点并填地址。
2. 编译装包：`cd rk3588_yolo && ./build.sh <名> && sudo ./install_app.sh <名>`（改了 C++ 必须重编译）。
3. **ROI 只在程序启动时加载**——改了 ROI 要在「程序管理」停止再启动，不能靠热重载。
4. 跑起来可以看左上角 overlay / 监看画面验证行为；上报的话：server 告警看本地发件箱 `ls <App>/alarm_store/`（Python 补传后即删），dify 看 `redis-cli lrange dify_queue 0 -1`。

## 常见坑（写之前过一遍）

- **类别名不匹配**：`r.label != "person"` 里的字符串必须和该模型 `labels.txt` 里的类名**完全一致**，否则永远不命中（表现为一直 CLEAR）。
- **坐标系搞错**：`ctx->roi` 和 `box` 都是模型 640 空间；别拿源分辨率（如 1920）的数去比，必偏, 参考已有的功能来编写相关ROI区域判定的功能。
- **上报地址硬编码**：别在逻辑里写死 `http://...`；地址走 `ctx->config->server_url`（方案2，跟着消息走，不同通道发不同服务器）。
- **每帧狂发**：上报/报警一定要用 `ctx->timestamp_ms` 限频或加闩锁，否则一秒几十条。
- **状态放错地方**：跨帧的东西放 `ctx->state`（每通道独立），不要用 `static` 局部变量（会被所有通道共享、串台）。
- **空指针**：`ctx->frame` / `ctx->results` / `ctx->roi` 可能为空，先判空。
