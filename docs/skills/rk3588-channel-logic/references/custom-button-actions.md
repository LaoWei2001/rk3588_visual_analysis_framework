# Web 实时画面的自定义按钮：实现原理与二次开发指南

本文说明「程序管理 → 实时画面 → 通道控制」里的自定义按钮是怎样从模块 `logic.json` 经生成清单到达 C++ 通道逻辑的，以及如何为一个 `logic_xxx` 增加新按钮。

它既是给二次开发者看的开发文档，也可以直接作为大模型修改本项目时的上下文。源码才是最终权威；文末列出了关键文件。

## 先记住结论

一个业务按钮由两部分组成：

1. 模块 `logic.json` 的 `actions` 负责声明按钮长什么样、发送什么动作名和固定参数；
2. 对应 `logic_xxx.cpp` 的 `ChannelActionFunc` 负责解释动作并修改本通道状态。

按钮不是硬编码在 React 页面里的。只要按约定修改模块目录的 `logic.json` 和对应 C++ logic，正常打包生成新的 App `logics.json` 后，Web 会动态渲染，通常不需要改前端和 FastAPI。不要直接修改生成文件。

最重要的名字对齐规则是：

```text
config.json 中通道的 logic
== REGISTER_LOGIC(logic_xxx) 的函数名
== REGISTER_LOGIC_ACTION(logic_xxx, ...) 的第一参数
== 生成 logics.json 的 channel_logics[].name
```

按钮本身还要满足：

```text
模块 logic.json 中 actions[].id == C++ 中 action.name 的判断字符串
```

## 整体链路

```text
App 启动
  process_manager.py 设置 RK_CHANNEL_CONTROL_SOCKET=<app>/run.control.sock
  C++ channel_control_init() 创建 Unix Socket 并启动控制线程

用户打开“实时画面”
  AppsPage.tsx
    → GET /api/apps/{app}/channel-actions
    → FastAPI 读取 assets/run.config 指向的配置文件
    → 按通道数组顺序找到当前 logic
    → 从 App 根目录 logics.json 取该 logic 的 actions[]
    → React 为每个通道动态渲染按钮

用户点击按钮
  AppsPage.tsx
    → 可选 confirm 确认框
    → POST /api/apps/{app}/channels/{channel_index}/actions/{action.id}
       body = {"payload": action.payload || {}}
    → FastAPI 检查 App 正在运行、run.control.sock 存在
    → 生成 request_id，经 Unix Socket 发送 JSON
    → C++ handle_client()
       ├─ 系统级动作：立即执行并返回
       └─ 业务级动作：记录当前 logic_name，放入该通道 FIFO 队列
    → HTTP 返回 accepted，Web 显示提示

下一次该通道处理逻辑帧
  invoke_channel_logic()
    → channel_control_take() 一次取走该通道全部待处理动作
    → 校验入队时 logic_name 仍等于当前 logic
    → 调用 REGISTER_LOGIC_ACTION 注册的 handler
    → 再调用正常的 logic_xxx(ctx)
```

这条链路的设计目的，是让 Socket 线程只负责收消息和入队，不直接并发修改 `ctx->state`。业务 handler 在该通道正常的逻辑处理上下文中执行，而且位于当帧 `logic_xxx(ctx)` 之前，因此可以安全访问本通道的 `ctx->state`、`ctx->frame`、`ctx->results` 等数据。

## 按钮声明：模块 `logic.json`

在 `rk3588_yolo/src/logic/modules/logic_xxx/logic.json` 中增加 `actions`：

```json
{
  "label": "演示逻辑",
  "report_fields": [],
  "actions": [
    {
      "id": "reset_counter",
      "label": "计数清零",
      "style": "danger",
      "confirm": "确定清空当前通道计数吗？",
      "help": "只清空当前通道的运行状态",
      "payload": {
        "reason": "manual",
        "keep_alarm_count": true
      }
    }
  ],
  "parameters": {"type": "object", "additionalProperties": false, "properties": {}}
}
```

### action 字段表

| 字段 | 必填 | 含义 |
|---|---:|---|
| `id` | 是 | 动作唯一标识。进入 URL，并原样成为 C++ 的 `action.name` |
| `label` | 否 | 按钮显示文字；缺省显示 `id` |
| `style` | 否 | `default` / `primary` / `danger`；只控制外观，不改变权限和行为 |
| `confirm` | 否 | 非空时，发送前调用浏览器 `window.confirm()` |
| `help` | 否 | 按钮的 `title` 提示；缺省显示 `id` |
| `payload` | 否 | 随按钮发送的固定 JSON 对象；C++ 中通过 `action.payload_json` 读取 |

约束和注意点：

- 同一 logic 的 `actions[].id` 应唯一，否则 React 的按钮 key 重复，C++ 也无法区分语义。
- `payload` 当前是声明在模块 `logic.json` 里的固定对象，Web 没有为每次点击动态输入参数的表单。要让用户临时输入，需另做前端交互。
- `style` 目前只有上述三个值，未知值不会获得对应按钮样式。
- `confirm` 只在 Web 点击时生效，外部 API 调用不会弹确认框，也不会由后端再次确认。
- 正常打包时 `build.sh` 聚合所有 `modules/*/logic.json`，在 App 根目录生成 `logics.json`。Web 后端读取已安装 App 的生成物；不要手工修改它。

## C++ handler：一个 logic 注册一个动作分发函数

一个 logic 的所有业务按钮共用一个 `ChannelActionFunc`。在函数里按 `action.name` 分支：

```cpp
#include "logic/core/logic_common.h"

#include <memory>
#include <string>

struct DemoState
{
    int count = 0;
    bool highlight = false;
};

static DemoState &demo_state(ChannelContext *ctx)
{
    if (!*ctx->state)
        *ctx->state = std::make_shared<DemoState>();
    return *std::static_pointer_cast<DemoState>(*ctx->state);
}

static ChannelActionResult logic_demo_action(ChannelContext *ctx,
                                              const ChannelAction &action)
{
    ChannelActionResult result;
    if (!ctx || !ctx->state)
    {
        result.message = "ctx is null";
        return result;
    }

    DemoState &state = demo_state(ctx);

    if (action.name == "reset_counter")
    {
        state.count = 0;
        result.handled = true;
        result.message = "counter reset";
        return result;
    }

    if (action.name == "toggle_highlight")
    {
        state.highlight = !state.highlight;
        result.handled = true;
        result.message = state.highlight ? "highlight enabled" : "highlight disabled";
        return result;
    }

    result.message = std::string("unsupported action: ") + action.name;
    return result;
}

static void logic_demo(ChannelContext *ctx)
{
    if (!ctx || !ctx->state)
        return;

    DemoState &state = demo_state(ctx);
    if (state.highlight)
        draw_text(ctx, "HIGHLIGHT", cv::Point(20, 30), cv::Scalar(0, 255, 255), 0.7, 2);

    // 正常的逐帧检测与状态更新……
}

REGISTER_LOGIC(logic_demo);
REGISTER_LOGIC_ACTION(logic_demo, logic_demo_action);
```

### 两个注册宏缺一不可

- `REGISTER_LOGIC` 把逐帧函数登记到普通 logic 注册表；
- `REGISTER_LOGIC_ACTION` 把按钮 handler 登记到独立的 action 注册表。

`REGISTER_LOGIC_ACTION` 的第一参数必须是传给 `REGISTER_LOGIC` 的同一个 logic 入口函数。只写 `REGISTER_LOGIC` 时，视频逻辑可以运行，但点击业务按钮会在入队前收到 `current logic has no action handler`。只写 action 注册而没有普通 logic，也不能形成可运行的通道逻辑。

`src/logic` 下的 `.cpp` 由 CMake 的 `aux_source_directory` 自动收集，因此新增独立 `logic_xxx.cpp` 后，不需要手工把文件名加入 CMakeLists。

### handler 应做什么

推荐 handler 只做快速、确定的状态变更：

- 设置/清除 `ctx->state` 中的标志；
- 重置计数器、状态机或计时器；
- 写入一个 `pending_xxx` 标志，让紧随其后的 `logic_xxx(ctx)` 完成绘制、告警或依赖当前帧的操作。

现成的 `modules/logic_button_demo/logic.cpp` 就采用这个模式：`trigger_alarm` 按钮只设置 `pending_alarm = true`，随后正常的逐帧函数调用 `report_alarm()`。这样按钮处理、画面业务和告警逻辑的职责更清楚。

不要在 handler 中执行长时间阻塞的网络请求、休眠或重量级任务。handler 与该通道当帧逻辑处在同一处理路径，阻塞会直接拖慢通道处理。

## payload：把按钮固定参数传给 C++

协议中的 `payload` 最终被序列化到：

```cpp
action.payload_json  // std::string，默认 "{}"
```

框架不替业务解释 payload。logic 自己用 cJSON 解析，并保证释放：

```cpp
#include "logic/core/logic_common.h"
#include "third_party/json/cJSON.h"

static ChannelActionResult logic_demo_action(ChannelContext *ctx,
                                              const ChannelAction &action)
{
    ChannelActionResult result;

    if (action.name != "set_threshold")
    {
        result.message = "unsupported action";
        return result;
    }

    cJSON *root = cJSON_Parse(action.payload_json.c_str());
    if (!root)
    {
        result.message = "invalid payload json";
        return result;
    }

    const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "value");
    if (!cJSON_IsNumber(value) || value->valuedouble < 0.0 || value->valuedouble > 1.0)
    {
        cJSON_Delete(root);
        result.message = "payload.value must be in [0, 1]";
        return result;
    }

    // 把运行时可变值写入 ctx->state，不要修改只读的 ctx->config。
    // demo_state(ctx).runtime_threshold = value->valuedouble;

    cJSON_Delete(root);
    result.handled = true;
    result.message = "threshold updated";
    return result;
}
```

C++ 控制端单次 `recv` 的缓冲上限目前约为 64 KiB，且没有循环读取或长度前缀。Unix `SOCK_STREAM` 不保留消息边界：超过上限一定存在截断风险，即使小于上限也理论上可能因分段到达而只读到部分 JSON。当前协议依赖“小型本地 JSON 通常一次完整到达”，payload 应保持小而明确，不要通过按钮传图片或大块二进制数据；若以后需要可靠传输大消息，应增加换行分帧循环读取或明确的长度前缀协议。

## 运行时语义：二次开发最容易误解的地方

### 1. HTTP 的 `accepted` 只代表成功入队

业务动作的 Socket 响应在入队后立即返回：

```json
{"ok":true,"message":"accepted"}
```

此时 C++ handler 可能还没执行。handler 返回的 `ChannelActionResult.handled/message` 当前只写入 C++ 日志，不会回传给已经结束的 HTTP 请求。因此：

- Web 显示“操作已进入队列”是准确语义；
- 不能把 HTTP 200 当成业务动作已经完成；
- 需要确认最终效果时，应观察画面、状态或 `[ChannelAction]` 日志；
- 如果业务必须同步返回最终结果，需要另行设计结果查询或异步事件机制，不能只改 handler 的 `message`。

系统级动作不同，例如 `infer_toggle` 在 Socket 线程中立即执行，HTTP 响应可直接返回实际的新状态。

### 2. 动作按稳定的配置通道 ID 路由

`GET channel-actions` 只返回一个身份字段 `channel_id`，它就是配置对象里的
`ch.id`，也是 HTTP/Socket、C++ 数组、logic、告警和录像共同使用的稳定标识。
配置数组顺序只影响显示宫格位置，不会产生新的通道号。

### 3. FIFO、容量和消费时机

- 每通道独立一个 FIFO 队列，互不串台；
- 单通道最多保留 64 个动作；满时静默丢弃最老的动作，再加入最新动作；
- 每次处理该通道 logic 前会一次性取走当前全部动作，并按 FIFO 顺序执行；
- Web 的 busy 状态只持续到 HTTP 请求结束，不是业务防抖。连续快速点击仍可能排入多个动作；
- 如果通道没有继续产生逻辑帧，业务动作就不会被消费；系统级动作不受此限制。

需要“只允许一次”“合并重复点击”或“冷却 N 秒”时，应在 handler / `ctx->state` 中明确实现，而不是依赖按钮禁用状态。

### 4. logic 热切换保护

业务动作入队时会记录当时的 `logic_name`。消费时如果通道已经切换到另一个 logic，旧动作会被丢弃并记录日志，避免把 `logic_A` 的 `reset` 误投给 `logic_B`。

### 5. 状态必须放在 `ctx->state`

`ctx->state` 是每通道独立的跨帧状态，也是按钮和逐帧函数共享状态的正确位置。不要用函数级或文件级 `static` 保存可变业务状态，否则同一个 logic 被多个通道复用时会串台。

`ctx->config` 是只读配置，不适合保存按钮改变的运行时值。若按钮修改的值还需要跨重启持久化，就不属于纯运行时 action，应另行设计配置写入和热重载流程。

## 系统级动作与业务级动作

### 业务级动作

绝大多数自定义按钮都应采用业务级动作：

- 在模块 `logic.json` 的该 logic 下声明；
- 由 `REGISTER_LOGIC_ACTION` handler 处理；
- 入队，在下一逻辑帧执行；
- 可安全读写 `ctx->state`，并访问当帧数据。

### 系统级动作

系统级动作不属于具体 logic，在 `channel_control.cpp::handle_client()` 中、查询当前 logic 之前直接处理。目前已有 `infer_toggle`。

系统级动作适合推理开关这类框架能力，特点是：

- 不进入业务动作队列；
- 不需要当前 logic 注册 action handler；
- 立即执行、立即响应；
- 修改共享 `ChannelState` 时必须使用项目已有的互斥锁。

如果希望系统级动作在某个 logic 的 Web 卡片上显示，仍需把相同 `id` 的 action 声明放进该 logic 的 `actions[]`。当前后端没有独立的全局 `system_actions` 合并逻辑。

除非需求确实是框架级、所有 logic 通用的控制，否则不要为了少写一个 handler 就把业务动作塞进 `handle_client()`。

## 从零新增一个按钮：最小步骤

假设要给 `logic_foo` 加“清空状态”按钮：

1. 在 `src/logic/modules/logic_foo/logic.json` 的 `actions` 增加：

   ```json
   {
     "id": "clear_state",
     "label": "清空状态",
     "style": "danger",
     "confirm": "确定清空当前通道状态吗？",
     "help": "清空计数、计时和告警闩锁"
   }
   ```

2. 在 `logic_foo.cpp` 实现或扩展统一 handler：

   ```cpp
   static ChannelActionResult logic_foo_action(ChannelContext *ctx,
                                                const ChannelAction &action)
   {
       ChannelActionResult result;
       if (!ctx || !ctx->state)
       {
           result.message = "ctx is null";
           return result;
       }

       if (action.name == "clear_state")
       {
           *ctx->state = std::make_shared<FooState>();
           result.handled = true;
           result.message = "state cleared";
           return result;
       }

       result.message = std::string("unsupported action: ") + action.name;
       return result;
   }
   ```

3. 在同一文件末尾确保同时存在：

   ```cpp
   REGISTER_LOGIC(logic_foo);
   REGISTER_LOGIC_ACTION(logic_foo, logic_foo_action);
   ```

4. 更新并重新安装程序包，使新的二进制和 App 根目录的 `logics.json` 同时生效；重新打开实时画面，让 Web 重新获取按钮清单。

普通新按钮不需要修改：

- `AppsPage.tsx`；
- `api/client.ts`；
- `web_console/backend/routers/channel_control.py`；
- `channel_control.cpp`。

只有新增动态输入 UI、全新按钮样式、协议字段或系统级动作时，才需要扩展这些框架文件。

## 外部 HTTP 调用

按钮使用的 API 也可以由 PLC 网关、扫码枪服务或其它程序调用。接口受 Web 登录鉴权保护：

```bash
# 登录获取 token
curl -X POST "http://<IP>:8080/api/auth/login" \
  -H "Content-Type: application/json" \
  -d '{"username":"root","password":"<password>"}'

# channel_id 使用 config.channels[].id
curl -X POST \
  "http://<IP>:8080/api/apps/<app>/channels/0/actions/clear_state" \
  -H "Authorization: Bearer <token>" \
  -H "Content-Type: application/json" \
  -d '{"payload":{"reason":"plc_reset"}}'
```

外部调用不要求 action 必须出现在 `logics.json`；后端会把 URL 中的 action 原样转发。但业务级动作仍要求当前 logic 已注册 handler，并由 handler 自己判断是否支持该 `action.name`。

## Web 显示规则

实时画面打开时，前端只获取一次通道控制清单。按钮状态规则如下：

- App 的 `run.control.sock` 不存在：所有按钮禁用，显示“未连接”；
- 通道配置 `enabled=false`：该通道按钮禁用；
- 当前按钮 HTTP 请求未结束：该按钮显示 `...` 并暂时禁用；
- 有 `confirm` 且用户取消：不发送请求；
- HTTP/Socket 返回错误：显示失败 toast。

如果运行中更换配置或修改 `logics.json`，已打开的弹窗不会自动刷新按钮清单。关闭并重新打开实时画面，或刷新页面。

### 关于当前的 `default action` 回退

当前 FastAPI 实现中，如果一个 logic 没有声明 `actions` 或数组为空，会生成一个 `id=default` 的回退按钮。它只是框架占位，不代表该 logic 一定支持 `default`：

- logic 没注册 handler：点击会在入队前返回 `current logic has no action handler`；
- logic 注册了 handler、但不处理 `default`：HTTP 仍可能先返回 `accepted`，最终日志显示 `handled=0`。

因此，正式业务逻辑应显式声明自己的 `actions[]`。若产品期望“未声明动作时完全不显示按钮”，应修改后端 `_default_actions()` / `get_channel_actions()` 的回退策略，而不是在各 logic 中被迫实现一个无意义的 `default`。

## 排错清单

| 现象 | 优先检查 |
|---|---|
| 实时画面没有预期按钮 | 已安装 App 根目录的 `logics.json` 是否更新；当前通道 `logic` 是否与 `channel_logics[].name` 完全一致；是否重新打开实时画面 |
| 按钮全部灰色并显示“未连接” | App 是否正在运行；`<app>/run.control.sock` 是否存在；日志是否有 `[ChannelControl] listening on ...` |
| 返回 `app is not running` | 程序管理中的 App 状态不是 running |
| 返回 `channel control socket not ready/unavailable` | C++ 控制服务未创建 Socket、App 刚启动尚未就绪，或 Socket 路径/权限异常 |
| 返回 `unknown channel_id` | URL 中的 ID 不存在、对应通道未启用，或修改配置后进程尚未重启 |
| 返回 `current logic has no action handler` | 漏写 `REGISTER_LOGIC_ACTION`；第一参数不是当前 logic 入口函数；新二进制未部署 |
| Web 提示 accepted，但画面没变化 | accepted 只是入队；通道是否仍有逻辑帧；handler 是否处理该 action；查 `[ChannelAction]` 日志 |
| 日志出现 `drop action ... queued for ...` | 动作入队后通道切换了 logic，框架按设计丢弃旧动作 |
| 日志 `handled=0 msg=unsupported action` | `actions[].id` 与 C++ `action.name` 分支不一致 |
| 多通道状态互相影响 | 使用了 `static` 可变状态；应迁移到每通道 `ctx->state` |
| 点击一次却执行多次 | 前端 busy 不是业务防抖；检查重复点击/外部调用，并在 state 中加闩锁、幂等键或冷却时间 |
| payload 无效 | `payload` 必须是 JSON 对象；检查字段类型、范围和 cJSON 释放路径 |

C++ 关键日志格式：

```text
[ChannelControl] listening on /opt/ai_apps/<app>/run.control.sock
[ChannelAction][ch00][logic_foo] action=clear_state request=... handled=1 msg=state cleared
[ChannelAction][ch00][logic_bar] drop action=reset request=... (queued for logic_foo)
```

## 给大模型的二次开发约束

把下面这段连同具体需求交给大模型，可减少它误改框架层的概率：

> 为本项目某个 `logic_xxx` 增加 Web 实时画面自定义按钮。先阅读本文、目标模块 C++ 和同目录 `logic.json`。普通业务按钮只修改目标模块，不要硬编码 React 按钮，不要修改 FastAPI 或 `channel_control.cpp`。保证 `actions[].id` 与 `action.name` 完全一致，同时让 `REGISTER_LOGIC_ACTION(logic_xxx, handler)` 的第一参数是已传给 `REGISTER_LOGIC(logic_xxx)` 的逻辑入口函数；源 `logic.json` 不写 `name`。所有跨帧、跨按钮的可变状态放在 `ctx->state`，禁止用可变 `static`，禁止修改只读 `ctx->config`。handler 只做快速状态变更，耗时工作不要阻塞通道逻辑；需要依赖当前帧的绘制/告警，优先设置 pending 标志并在紧随其后的 `logic_xxx(ctx)` 中处理。若使用 payload，校验 JSON 类型和范围并正确释放 cJSON。完成后说明按钮声明、handler 分支、状态变化、是否异步入队以及验证观察点。

模型实施前还应回答：

1. 这是业务级动作还是确实需要框架级的系统动作？
2. action ID、按钮文字、样式、确认提示和 payload 分别是什么？
3. 重复点击是累计、幂等、切换，还是需要冷却/去重？
4. 动作只改变运行时状态，还是必须跨重启持久化？
5. 最终效果通过画面、告警、状态还是日志确认？

## 源码地图

| 文件 | 职责 |
|---|---|
| `rk3588_yolo/src/logic/modules/logic_xxx/logic.json` | 当前 logic 的按钮元数据源；打包后聚合给 Web |
| `rk3588_yolo/src/logic/modules/logic_button_demo/logic.cpp` | 自定义按钮最完整的现成示例：画面、切换、清空、手动告警 |
| `rk3588_yolo/src/logic/core/channel_logic.h` | `ChannelAction`、`ChannelActionResult`、handler 类型和注册宏 |
| `rk3588_yolo/src/logic/core/channel_logic.cpp` | 普通 logic 与 action handler 两套注册表 |
| `rk3588_yolo/src/control/channel_control.cpp` | Unix Socket 协议、系统动作、每通道队列和控制线程 |
| `rk3588_yolo/src/analyzer/channel_pipeline.cpp` | 每帧取队列、logic 名校验、执行 handler 后再执行逐帧 logic |
| `rk3588_yolo/src/main.cpp` | 控制服务初始化与退出清理 |
| `web_console/backend/services/process_manager.py` | 启动 App 时设置 `RK_CHANNEL_CONTROL_SOCKET` |
| `web_console/backend/routers/channel_control.py` | 动作清单 API、HTTP 到 Unix Socket 的桥接 |
| `web_console/frontend/src/api/client.ts` | action TypeScript 类型与 API 封装 |
| `web_console/frontend/src/pages/AppsPage.tsx` | 实时画面按钮渲染、确认、busy 状态和结果提示 |
| `rk3588_yolo/build.sh` | 校验并聚合 `src/logic/modules/*/logic.json`，生成 App 根目录 `logics.json` |

相关文档：

- `channelcontext-api.md`：handler 和逐帧 logic 可访问的 `ctx` 数据；
- `logic-naming-and-registration.md`：logic 名称与自注册机制；
- `adding-config-parameter.md`：需要持久化、可热重载的配置参数；
- `upload-and-wiring.md`：按钮触发告警/上报时的接线方式。
