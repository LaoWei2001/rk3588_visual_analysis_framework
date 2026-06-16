---
name: rk3588-global-logic
description: >-
  Implements a new "global logic" (a global_xxx function) for the RK3588 YOLO
  vision system — cross-channel / multi-camera strategies that run on their own
  polling thread and can see ALL channels at once. Use this skill whenever a
  developer describes a rule that spans MULTIPLE channels or aggregates across
  the whole system — e.g. "统计所有摄像头里的总人数超过 N 就报警", "两个及以上区域同时
  有人就联动报警", "把多路画面里最新出现目标的那一路推给服务器", "全局周期巡检 / 汇总上报",
  "cross-camera handoff / 跨摄像头联动". Also trigger on any edit to
  global_logic.cpp / GlobalContext / g_global_logic_registry, or when wiring a
  global logic into config.json (global_logics) / logics.json (global_logics).
  Trigger even if they don't say "global" — if the rule needs to look at more
  than one channel at the same time, use this skill. For a SINGLE channel's own
  detection/alarm rule use rk3588-channel-logic instead; for web console /
  deploy / 后台服务 use rk3588-console-ops.
---

# RK3588 全局逻辑（global logic）开发

把开发者一句"跨多路/全局"的需求，落成 `global_xxx(GlobalContext *gctx)`——一个在**独立线程里按周期轮询**、能**一次看到所有通道**的函数，并接好线（前端可选、可上报）。开发者只需验证。

> **先分清该用哪个 skill**：只看**一路自己**的目标/计时/告警 → `rk3588-channel-logic`（每帧调用，`ctx` 是本通道当帧）。需要**同时看多路**或做**跨路关联/全局汇总** → 本 skill。控制台/部署/后台服务 → `rk3588-console-ops`。

## 你的产出（output）

针对一个跨通道需求，给出一组对齐现有约定的改动：

1. **`rk3588_yolo/src/logic/global_logic.cpp`** — 新增 `static void global_xxx(GlobalContext *gctx)`；并在 `global_logic_register()` 里（约第 65 行 `/* 新增 global logic: ... */` 标记处）加一行 `register_global_logic("global_xxx", global_xxx);`。若函数定义写在 `global_logic_register()` 之后，记得在文件顶部（`global_example`/`global_default` 前置声明附近）补一行前置声明。
2. **`rk3588_yolo/src/logic/logics.json`** — `global_logics` 数组加一条 `{ "name": "global_xxx", "label": "中文名", "params": [] }`，让网页「全局逻辑配置」面板能选到它。
3. **接线说明**给开发者：在 `config.json` 的 `global_logics` 数组里加一个实例（或在网页「全局逻辑配置」面板「+ 添加」），填 `enable / logic / channels / poll_interval_ms`；改完怎么编译验证。

> 不要直接改某台设备的 `config.json`（那是运行配置，由网页编辑器/部署管理）。你只改源码 + logics.json + 给接线说明。

## 核心心智模型（先理解这些，代码自然就对）

- **一个 `global_logics` 实例 = 一个 `global_xxx` 函数 + 一条独立 pthread**。线程按 `poll_interval_ms` 轮询调用你的函数（`global_logic.cpp` 的 `global_logic_thread_func`）。支持多个实例并行，**互不影响**（上限 `MAX_GLOBAL_LOGICS=16`）。
- **不是"每帧"，是"每 tick"**：你拿不到"当帧"引用，而是用 `gctx->get_channel_snapshot(ch)` 取**某通道的原子快照**（可能比最新帧旧几帧——用 `result_age_ms` 自检新鲜度）。
- **天然能看所有通道**：`gctx->for_each_channel(...)` 遍历本实例监控的通道；`channels` 为空数组=监控全部活跃通道。
- **跨 tick 记忆放 `gctx->state`**：本实例独有的一格 `shared_ptr`（计数、闩锁、上次告警时间都放这）。**别用 `static` 局部变量**（会被所有实例共享、串台）。
- **取数要原子、要同帧**：`get_channel_snapshot()` 在一把 `chn_mtx` 锁内一次性读出 `frame + results + logic_state + frame_seq + result_age_ms`，因此 `snapshot.frame` 与 `snapshot.results` **必定同帧**。**不要**把它拆成多次调用分别取帧和结果——那样无法保证同帧。
- **上报地址同 channel logic（方案2）**：需要上报时调 `alarm_uploader_enqueue(...)` / `dify_uploader_enqueue(...)`；地址不要硬编码（细节见 `rk3588-channel-logic` 的 `references/upload-and-wiring.md`）。

`GlobalContext` 的**完整字段与方法**（`get_channel_snapshot` / `for_each_channel` / `channel_has_target` / `get_channel_target_count` / `has_new_infer` / `latest_infer_channel` / `tick_id` …）在 **`rk3588_yolo/src/logic/global_logic.h`**——该头文件自带详尽注释 + 最小示例，写之前先读它。现成范例见 `global_logic.cpp` 里的 `global_example`（多区域同时有人就联动告警）和 `global_default`（周期巡检打印各通道状态）。

## 写全局逻辑的骨架（参考）

```cpp
// 跨 tick 状态：本实例独有（计数/闩锁/限频都放这）
struct MyGlobalState {
    uint64_t last_alarm_ms = 0;
};

static void global_xxx(GlobalContext *gctx)
{
    if (!gctx) return;

    // 1) 惰性初始化本实例跨 tick 状态（只写 *gctx->state，不要 reset 指针本身）
    if (!*gctx->state) *gctx->state = std::make_shared<MyGlobalState>();
    auto &st = *std::static_pointer_cast<MyGlobalState>(*gctx->state);

    // 2) 可选：没有新推理结果就跳过本 tick，省算力
    if (!gctx->has_new_infer) return;

    // 3) 跨通道聚合（坐标都在模型 640 空间；snapshot.frame 与 .results 同帧）
    int total = 0;
    gctx->for_each_channel([&](int ch, int /*idx*/) {
        ChannelSnapshot s = gctx->get_channel_snapshot(ch);
        if (s.frame.empty() || s.result_age_ms > 500) return;   // 新鲜度自检
        for (const auto &r : s.results)
            if (r.label == "person") ++total;
    });

    // 4) 触发 + 限频（别每 tick 都发）
    if (total >= 10 && gctx->timestamp_ms - st.last_alarm_ms > 5000) {
        st.last_alarm_ms = gctx->timestamp_ms;
        printf("[global_xxx] ALARM total person=%d\n", total);
        // alarm_uploader_enqueue(...) ← 按需接入上报
    }
}
```

## 🔑 多通道共用结构体，逻辑变量为什么不会串台（隔离机制）

这是本项目最关键、也最容易误解的一点：**`ChannelContext`、`ChannelConfig`、`GlobalContext` 都只是"类型/壳子"，被所有通道/实例复用；真正的数据各自独立，靠"按索引分槽"+"每次现构造"实现隔离。**

### 1. 逻辑函数是无状态的"纯代码"，状态全在外面

`logic_person_alarm`、`global_xxx` 这些函数本身**不存任何数据**——它们是一段被反复调用的代码。"通道 0 的人数"和"通道 1 的人数"不在函数里，而在**框架持有的、按通道号分槽的存储**里。函数每次被调用时，框架把"本通道/本实例的那一槽"通过 `ctx`/`gctx` 指针递给它。

### 2. 每通道一套独立存储，用 `chnId` 当下标隔离

全局只有一个 `g_pCtrl`，但它内部是**数组**：

```
g_pCtrl->config.channels[chnId]      // 每通道一个 ChannelConfig 实例（同类型，不同对象）
g_pCtrl->channels_state[chnId]       // 每通道一个 ChannelState：装 logic_state / roi_for_logic /
                                     //   last_results / last_frame / fps / frame_seq …
g_pCtrl->chn_mtx[chnId]              // 每通道一把锁
g_process_mtx[chnId]                 // 每通道一把"逻辑处理"锁
```

`ChannelConfig` 是**类型**；`channels[0]` 和 `channels[1]` 是**两个不同的对象**。所以"共用 ChannelConfig"指的是共用**结构定义**，不是共用**同一份数据**——下标不同，物理内存就不同，天然不串。

### 3. `ChannelContext` 是"每次调用现搭的栈上壳子"，不是共享单例

每帧处理时，`channel_pipeline.cpp` 的 `invoke_channel_logic()` 会**在栈上新建一个 `ChannelContext ctx;`**，把它的指针**对准本通道的那一槽**后才调用逻辑：

```cpp
ChannelContext ctx;                                   // 本次调用专属，调用完即销毁
ctx.chnId   = chnId;
ctx.config  = &g_pCtrl->config.channels[chnId];       // ← 指向本通道 config
ctx.results = &current_results;                        // ← 本帧检测结果（本地变量）
ctx.roi     = &ch_state.roi_for_logic;                 // ← 本通道 ROI
ctx.state   = &ch_state.logic_state;                   // ← 本通道跨帧状态槽
ctx.infer_enabled = infer_enabled;
fn(&ctx);                                              // 同一个 fn，喂不同的 ctx
```

所以同一个 `logic_xxx` 被 8 个通道调用，是"**同一段代码 + 8 个各自指向自家数据的 ctx**"。`ctx` 本身不共享（栈上、用完就扔），共享的只是它的**类型**。

### 4. 跨帧记忆：`ctx->state` 是"每通道一格"的 `shared_ptr<void>`

逻辑要记住上一帧的东西（计时、闩锁、track 去重表）时，放进 `ctx->state`（即 `channels_state[chnId].logic_state`）。每个通道**第一次**用时各自 `make_shared` 自己的状态对象：

```cpp
if (!*ctx->state) *ctx->state = std::make_shared<XxxState>();   // 通道 0、通道 1 各建各的
auto &s = *std::static_pointer_cast<XxxState>(*ctx->state);
```

于是通道 0 的 `XxxState` 与通道 1 的 `XxxState` 是**两个对象**，互不可见。这就是隔离的落点。

### 5. 会破坏隔离的唯一常见写法：`static` 局部变量

```cpp
static int counter = 0;   // ❌ 整个进程只有一份，被所有通道共享 → 立刻串台
counter++;
```

框架给了 `ctx->state` 就是为了取代它。**跨帧的东西一律放 `ctx->state`，绝不要用 `static`/全局变量当每通道状态。**

### 6. 并发安全：不同通道分槽、同通道串行

各通道的推理/逻辑可能在不同线程跑，但：①它们写的是**不同下标**的槽，本就不冲突；②同一通道的两条处理路径（推理完成 / 非推理直通）用 `g_process_mtx[chnId]` 串行；③对共享状态的最终写回在 `chn_mtx[chnId]` 内完成。所以"同帧"和"不串台"同时成立。

### 7. 全局逻辑的隔离：每实例一套 `gctx->state`

`global_logics` 配多个实例时，每个实例是一条独立线程、一个独立的 `GlobalLogicThread`，各自持有自己的 `state`（`gctx->state` 指向本实例那一份）。实例之间互不影响。全局逻辑**读**通道数据只走 `get_channel_snapshot(ch)`——它返回**深拷贝快照**，不会别名/写到通道自有内存，因此既线程安全又不会污染通道状态。

> 一句话总结：**结构体是共享的"模板"，数据是按 `chnId`/实例分槽的"实例"；`ctx`/`gctx` 每次调用现搭、只指向自家那一槽；跨帧状态放 `ctx->state`/`gctx->state`，别用 `static`。**

## 接线三件套（写完逻辑必做）

1. **注册**（global_logic.cpp，`global_logic_register()` 内的标记处）：
   ```cpp
   register_global_logic("global_xxx", global_xxx);
   ```
   （函数定义在该函数之后的话，记得在文件顶部加前置声明 `static void global_xxx(GlobalContext *gctx);`）
2. **声明到 logics.json 的 `global_logics`**（让网页「全局逻辑配置」面板能选到）：
   ```json
   { "name": "global_xxx", "label": "中文名", "params": [] }
   ```
   网页下拉的可选项来自后端透传的 `known_global_logics`（即 logics.json 的 `global_logics`）。
3. **加一个运行实例**到 `config.json` 的 `global_logics` 数组（或网页「全局逻辑配置」面板「+ 添加」）：
   ```json
   "global_logics": [
     { "enable": true, "logic": "global_xxx", "channels": [0,1,2], "poll_interval_ms": 200 }
   ]
   ```
   - `channels`: 空数组 = 监控所有活跃通道。
   - `poll_interval_ms`: 轮询周期；框架会按受监控通道的 `max_fps` 自动把它收紧到实时档（见 `global_logic_thread_func`）。

## 给开发者的收尾说明（每次都要附上）

1. 编译装包：`cd rk3588_yolo && ./build.sh <名> && sudo ./install_app.sh <名>`（改了 C++ 必须重编）。
2. 在 `config.json`/网页里加好 `global_logics` 实例并选中 `global_xxx`，重启程序。
3. 启动日志能看到 `[GlobalLogic] Thread started: logic=global_xxx ...` 和 `Started N/M instance(s)`；逻辑里的 `printf` 会周期打印；上报的话：server 看本地发件箱 `ls <App>/alarm_store/`，dify 看 `redis-cli lrange dify_queue 0 -1`。
4. **热重载注意**：`global_logics` 改动由 `config_monitor` 重启受影响实例；但通道数量/源地址等仍需重启程序。

## 常见坑（写之前过一遍）

- **用 `static` 当每实例/每通道状态**：必串台。跨 tick 状态放 `gctx->state`。
- **快照拆成多次取**：`get_channel_snapshot()` 必须一次取全，分次取无法保证 frame 与 results 同帧。
- **不做新鲜度检查**：务必 `if (s.frame.empty() || s.result_age_ms > 阈值) return;`，否则会用到很旧的结果。
- **每 tick 狂发**：上报/告警用 `gctx->timestamp_ms` 限频或加闩锁。
- **坐标系**：`snapshot.results[].box` 在模型 640 空间，别拿源分辨率算。
- **逻辑名没注册/没声明**：`register_global_logic` 漏了 → 框架回退 `global_default` 并告警；logics.json 漏了 → 网页选不到。
- **耗时过长**：`global_xxx` 执行时间要远小于 `poll_interval_ms`，否则拖慢轮询。
