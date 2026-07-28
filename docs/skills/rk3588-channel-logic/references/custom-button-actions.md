# Web 实时画面的自定义按钮：实现原理与二次开发指南

本文说明「程序管理 → 实时画面 → 通道控制」里的自定义按钮是怎样从模块 `logic.json` 经生成清单到达 C++ 通道逻辑的，以及如何为一个 `logic_xxx` 增加新按钮。

它既是给二次开发者看的开发文档，也可以直接作为大模型修改本项目时的上下文。源码才是最终权威；文末列出了关键文件。

阅读建议：

- 初学者先阅读“五分钟入门”和“action 字段表”，然后运行仓库现有的 `logic_button_demo`；
- 开发普通按钮时继续阅读“C++ handler 的编写规则”和“从零新增一个按钮”；
- 排查问题或扩展框架时，再阅读整体链路、运行时语义和系统级动作。

## 先记住结论

一个业务按钮由两部分组成：

1. 模块 `logic.json` 的 `actions` 负责声明按钮长什么样、发送什么动作名和固定参数；
2. 对应模块 `logic.cpp` 中的 `ChannelActionFunc` 负责解释动作并修改本通道状态。

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
模块 logic.json 中 actions[].id == C++ 中 action->name 的判断字符串
```

## 初学者先理解这一句话

```text
Web 按钮发送 action ID
    → C++ handler 根据 action->name 修改 ctx->state
    → 紧随其后的逐帧 logic 读取新状态并更新画面或业务结果
```

- `action`：一次按钮操作，例如 `increment`；
- `handler`：处理按钮操作的 C++ 函数；
- `ctx->state`：当前通道跨帧保存的运行时状态。

按钮不会直接调用某一段任意 C++ 代码。它先发送一个动作名，再由当前 logic 注册的 handler 判断该动作应该做什么。

## 五分钟入门：给画面中的数字增加 `+1` / `-1` 按钮

仓库已经提供完整示例：

```text
vision_analysis/src/logic/modules/logic_button_demo/
├── logic.cpp
└── logic.json
```

### 第一步：在 `logic.json` 声明按钮

```json
{
  "label": "按钮加减数字演示",
  "parameters": {
    "type": "object",
    "additionalProperties": false,
    "properties": {}
  },
  "report_fields": [],
  "actions": [
    {
      "id": "increment",
      "label": "+1",
      "style": "primary",
      "help": "将当前通道画面中的数字加 1。"
    },
    {
      "id": "decrement",
      "label": "-1",
      "style": "default",
      "help": "将当前通道画面中的数字减 1，最小值保持为 1。"
    }
  ]
}
```

这里最重要的是 `id`。点击 `+1` 后，C++ 收到的就是：

```cpp
action->name == "increment"
```

### 第二步：在 `logic.cpp` 处理按钮

下面是与上述 `logic.json` 对应的完整教学版代码：

```cpp
#include "logic/core/logic_common.h"

#include <climits>
#include <memory>
#include <string>

struct ButtonDemoState
{
    int number = 1;
};

static ButtonDemoState &button_demo_state(ChannelContext *ctx)
{
    if (!*ctx->state)
        *ctx->state = std::make_shared<ButtonDemoState>();
    return *std::static_pointer_cast<ButtonDemoState>(*ctx->state);
}

static ChannelActionResult logic_button_demo_action(
    ChannelContext *ctx, const ChannelAction *action)
{
    ChannelActionResult result;
    if (!ctx || !ctx->state || !action)
    {
        result.message = "ctx or action is null";
        return result;
    }

    ButtonDemoState &state = button_demo_state(ctx);

    if (action->name == "increment")
    {
        if (state.number < INT_MAX)
            ++state.number;
        result.handled = true;
        result.message = "number=" + std::to_string(state.number);
        return result;
    }

    if (action->name == "decrement")
    {
        if (state.number > 1)
            --state.number;
        result.handled = true;
        result.message = "number=" + std::to_string(state.number);
        return result;
    }

    result.message = "unsupported action: " + action->name;
    return result;
}

static void logic_button_demo(ChannelContext *ctx)
{
    if (!ctx || !ctx->state)
        return;

    ButtonDemoState &state = button_demo_state(ctx);
    const std::string text = std::to_string(state.number);
    draw_text(ctx, text.c_str(), cv::Point(300, 320),
              cv::Scalar(0, 255, 255), 3.0, 5);
}

REGISTER_LOGIC(logic_button_demo);
REGISTER_LOGIC_ACTION(logic_button_demo, logic_button_demo_action);
```

这个例子中：

1. `logic.json` 决定 Web 显示哪几个按钮；
2. `logic_button_demo_action()` 处理按钮；
3. handler 修改 `ctx->state` 中的 `number`；
4. `logic_button_demo()` 每帧把最新数字画出来；
5. 每个通道拥有自己的 `ctx->state`，所以多个通道不会共用同一个数字。

### 第三步：记住两个名字必须一致

```text
logic.json 的 actions[].id
== C++ handler 中判断的 action->name
```

例如：

```text
"id": "increment"
== action->name == "increment"
```

如果拼写不同，按钮仍可能显示并成功入队，但 handler 最终会返回 `unsupported action`。

## action 字段表

| 字段 | 必填 | 含义 |
|---|---:|---|
| `id` | 是 | 动作唯一标识。进入 URL，并原样成为 C++ 的 `action->name` |
| `label` | 否 | 按钮显示文字；缺省显示 `id` |
| `style` | 否 | `default` / `primary` / `danger`；只控制外观，不改变权限和行为 |
| `confirm` | 否 | 非空时，Web 点击后先调用浏览器 `window.confirm()` |
| `help` | 否 | 鼠标停留时的提示；缺省显示 `id` |
| `payload` | 否 | 随按钮发送的固定 JSON 对象；C++ 通过 `action->payload_json` 读取 |

约束和注意点：

- 同一 logic 的 `actions[].id` 必须唯一；重复时生成清单的构建校验会失败。
- `payload` 是声明在模块 `logic.json` 里的固定对象。当前 Web 没有为每次点击动态输入参数的表单；需要临时输入时必须扩展前端。
- `style` 目前只有 `default`、`primary`、`danger` 三种；未知值不会获得对应样式。
- `confirm` 只在 Web 点击时生效。外部 API 调用不会弹出确认框，后端也不会再次确认。
- 正常打包时，`build.sh` 会聚合所有 `modules/*/logic.json`，在 App 根目录生成 `logics.json`。不要手工修改生成文件。

## 整体链路（进阶）

```text
App 启动
  process_manager.py 设置 RK_CHANNEL_CONTROL_SOCKET=<app>/run.control.sock
  C++ channel_control_init() 创建 Unix Socket 并启动控制线程

用户打开“实时画面”
  AppsPage.tsx
    → GET /api/apps/{app}/channel-actions
    → FastAPI 读取 run.config 指定的 assets/<配置文件>
    → 过滤 enable=false 的通道，并按 channel_id 排序
    → 从 App 根目录 logics.json 取得当前 logic 的 actions[]
    → React 为每个启用通道动态渲染按钮

用户点击按钮
  AppsPage.tsx
    → 可选 confirm 确认框
    → POST /api/apps/{app}/channels/{channel_id}/actions/{action.id}
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

这条链路让 Socket 线程只负责收消息和入队，不直接并发修改 `ctx->state`。业务 handler 在该通道正常的逻辑处理路径中执行，并且位于当帧 `logic_xxx(ctx)` 之前，所以可以安全访问本通道的 `ctx->state`、`ctx->frame` 和 `ctx->results`。

## C++ handler 的编写规则

### 一个 logic 只注册一个业务动作 handler

同一个 logic 的所有业务按钮共用一个 `ChannelActionFunc`，在函数中按照 `action->name` 分支处理。`action` 由框架传入，但指针接口仍应先检查是否为空。

### 业务按钮需要两个注册宏

- `REGISTER_LOGIC` 把逐帧函数登记到普通 logic 注册表；
- `REGISTER_LOGIC_ACTION` 把业务按钮 handler 登记到动作注册表。

`REGISTER_LOGIC_ACTION` 的第一参数必须是传给 `REGISTER_LOGIC` 的同一个 logic 入口函数：

```cpp
REGISTER_LOGIC(logic_demo);
REGISTER_LOGIC_ACTION(logic_demo, logic_demo_action);
```

只写 `REGISTER_LOGIC` 时，视频逻辑可以运行，但点击业务按钮会在入队前收到 `current logic has no action handler`。系统级动作是例外，例如现有的 `infer_toggle` 由框架直接处理，不需要业务 handler。

业务模块应保持完整目录结构：

```text
vision_analysis/src/logic/modules/logic_xxx/
├── logic.cpp
└── logic.json
```

`src/logic` 下的 `.cpp`、`.cc` 和 `.cxx` 由 CMake 的 `file(GLOB_RECURSE ...)` 递归收集，因此不需要手工把新文件名写进 `CMakeLists.txt`。但是每个业务模块仍必须提供 `logic.json`，并且模块中恰好注册一个普通 logic。

### handler 应该做什么

推荐 handler 只进行快速、确定的状态变更：

- 设置或清除 `ctx->state` 中的标志；
- 重置计数器、状态机或计时器；
- 写入一个 `pending_xxx` 标志，让紧随其后的 `logic_xxx(ctx)` 完成绘制、告警或其它依赖当前帧的操作。

不要在 handler 中执行长时间阻塞的网络请求、休眠或重量级任务。handler 与该通道当帧 logic 位于同一处理路径，阻塞会直接拖慢通道处理。

## payload：把按钮固定参数传给 C++

先在模块 `logic.json` 中为按钮声明固定参数：

```json
{
  "id": "set_threshold",
  "label": "设置阈值",
  "style": "primary",
  "payload": {
    "value": 0.6
  }
}
```

协议中的 `payload` 最终被序列化到：

```cpp
action->payload_json  // std::string，默认 "{}"
```

框架不替业务解释 payload。logic 自己用 cJSON 解析，并保证释放：

```cpp
#include "logic/core/logic_common.h"
#include "third_party/json/cJSON.h"

static ChannelActionResult logic_demo_action(ChannelContext *ctx,
                                              const ChannelAction *action)
{
    ChannelActionResult result;
    if (!ctx || !ctx->state || !action)
    {
        result.message = "ctx or action is null";
        return result;
    }

    if (action->name != "set_threshold")
    {
        result.message = "unsupported action";
        return result;
    }

    cJSON *root = cJSON_Parse(action->payload_json.c_str());
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

    // 假设 DemoState 已定义 runtime_threshold 字段。
    // 把运行时可变值写入 ctx->state，不要修改只读的 ctx->config。
    demo_state(ctx).runtime_threshold = value->valuedouble;

    cJSON_Delete(root);
    result.handled = true;
    result.message = "threshold updated";
    return result;
}
```

### 进阶：payload 大小限制

C++ 控制端单次 `recv` 的缓冲上限目前约为 64 KiB，且没有循环读取或长度前缀。Unix `SOCK_STREAM` 不保留消息边界：超过上限一定存在截断风险，即使小于上限也理论上可能因分段到达而只读到部分 JSON。

因此 payload 应保持小而明确，不要通过按钮传图片或大块二进制数据。若以后需要可靠传输大消息，应增加换行分帧循环读取或明确的长度前缀协议。

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

- 视频宫格位置可以受配置数组顺序影响；
- Web 的通道控制卡片只包含 `enable=true` 的通道，并按 `channel_id` 排序；
- 配置数组下标不是新的通道号，调用 API 时必须使用 `config.channels[].id`。

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

`ctx->state` 是运行时状态，不是永久存储。进程重启或该通道切换到其它 logic 时，当前 logic 的状态会被清除。

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

1. 在 `vision_analysis/src/logic/modules/logic_foo/logic.json` 的 `actions` 增加：

   ```json
   {
     "id": "clear_state",
     "label": "清空状态",
     "style": "danger",
     "confirm": "确定清空当前通道状态吗？",
     "help": "清空计数、计时和告警闩锁"
   }
   ```

2. 在 `vision_analysis/src/logic/modules/logic_foo/logic.cpp` 实现或扩展统一 handler：

   ```cpp
   static ChannelActionResult logic_foo_action(ChannelContext *ctx,
                                                const ChannelAction *action)
   {
       ChannelActionResult result;
       if (!ctx || !ctx->state || !action)
       {
           result.message = "ctx or action is null";
           return result;
       }

       if (action->name == "clear_state")
       {
           *ctx->state = std::make_shared<FooState>();
           result.handled = true;
           result.message = "state cleared";
           return result;
       }

       result.message = std::string("unsupported action: ") + action->name;
       return result;
   }
   ```

3. 在同一文件末尾确保同时存在：

   ```cpp
   REGISTER_LOGIC(logic_foo);
   REGISTER_LOGIC_ACTION(logic_foo, logic_foo_action);
   ```

4. 运行项目正常的构建或打包流程，确认模块清单校验通过；更新并重新安装程序包，使新的二进制和 App 根目录生成的 `logics.json` 同时生效。最后重新打开实时画面，让 Web 重新获取按钮清单。

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

外部调用不要求 action 必须出现在 `logics.json`；后端会把 URL 中的 action 原样转发。但业务级动作仍要求当前 logic 已注册 handler，并由 handler 自己判断是否支持该 `action->name`。

## Web 显示规则

实时画面打开时，前端只获取一次通道控制清单。按钮状态规则如下：

- App 的 `run.control.sock` 不存在：所有按钮禁用，显示“未连接”；
- 通道配置 `enable=false`：该通道不会出现在通道控制列表中；
- 当前按钮 HTTP 请求未结束：该按钮显示 `...` 并暂时禁用；
- 有 `confirm` 且用户取消：不发送请求；
- HTTP/Socket 返回错误：显示失败 toast。

如果运行中更换配置或重新安装了包含新版 `logics.json` 的程序包，已打开的弹窗不会自动刷新按钮清单。关闭并重新打开实时画面，或刷新页面。

### 未声明动作时不显示按钮

FastAPI 只返回当前 logic 在 App 根目录 `logics.json` 中显式声明的 `actions[]`：

- logic 没有声明 `actions` 或数组为空：Web 不显示按钮；
- logic 声明了 action、但没有注册 C++ handler：按钮仍会显示，点击后返回 `current logic has no action handler`；
- logic 注册了 handler、但不处理该 action ID：HTTP 可能先返回 `accepted`，最终日志显示 `handled=0`。

因此，按钮是否显示由 `logic.json` 的 `actions[]` 决定，按钮是否具备实际功能由对应的 `REGISTER_LOGIC_ACTION` handler 决定。

## 排错清单

| 现象 | 优先检查 |
|---|---|
| 实时画面没有预期按钮 | 通道是否为 `enable=true`；已安装 App 根目录的 `logics.json` 是否更新；当前通道 `logic` 是否与 `channel_logics[].name` 完全一致；是否重新打开实时画面 |
| 按钮全部灰色并显示“未连接” | App 是否正在运行；`<app>/run.control.sock` 是否存在；日志是否有 `[ChannelControl] listening on ...` |
| 返回 `app is not running` | 程序管理中的 App 状态不是 running |
| 返回 `channel control socket not ready/unavailable` | C++ 控制服务未创建 Socket、App 刚启动尚未就绪，或 Socket 路径/权限异常 |
| 返回 `unknown channel_id` | URL 使用的是否为 `config.channels[].id`；该 ID 是否存在于当前进程加载的配置；修改配置后进程是否已重启 |
| 返回 `current logic has no action handler` | 漏写 `REGISTER_LOGIC_ACTION`；第一参数不是当前 logic 入口函数；新二进制未部署 |
| Web 提示 accepted，但画面没变化 | accepted 只是入队；通道是否启用并仍有逻辑帧；handler 是否处理该 action；查 `[ChannelAction]` 日志 |
| 日志出现 `drop action ... queued for ...` | 动作入队后通道切换了 logic，框架按设计丢弃旧动作 |
| 日志 `handled=0 msg=unsupported action` | `actions[].id` 与 C++ `action->name` 分支不一致 |
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

下面的模板可以和具体需求一起交给大模型。使用时先替换尖括号中的内容：

```text
目标：
为 <logic 名称> 增加 Web 实时画面的自定义按钮。

按钮需求：
- action ID：<例如 clear_state>
- 显示文字：<例如 清空状态>
- style：<default / primary / danger>
- confirm：<不需要则写无>
- payload：<不需要则写 {}>
- 重复点击语义：<累计 / 幂等 / 切换 / 冷却>
- 最终可观察效果：<画面 / 告警 / 状态 / 日志>

实施要求：
1. 先完整阅读：
   - docs/skills/rk3588-channel-logic/references/custom-button-actions.md
   - vision_analysis/src/logic/modules/<目标模块>/logic.cpp
   - vision_analysis/src/logic/modules/<目标模块>/logic.json
   如果文档与当前源码冲突，以当前源码为准。
2. 普通业务按钮只修改目标模块的 logic.cpp 和 logic.json。不要把按钮硬编码进 React，
   不要修改 FastAPI、channel_control.cpp 或其它无关模块。
3. 修改源模块的 logic.json；不要直接修改构建生成或已安装 App 根目录的 logics.json，
   也不要在源 logic.json 中添加 name。
4. 保证 actions[].id 与 C++ 的 action->name 判断字符串完全一致。
5. 保证 REGISTER_LOGIC_ACTION(logic_xxx, handler) 的第一参数与
   REGISTER_LOGIC(logic_xxx) 使用同一个 logic 函数名。
6. 复用目标模块现有的状态结构。所有跨帧、跨按钮的可变状态放进 ctx->state；
   禁止使用可变 static，禁止修改只读的 ctx->config。
7. handler 只做快速状态变更。需要当前帧才能完成的绘制、告警或上报，应设置
   pending 标志，再由紧随其后的逐帧 logic 处理。
8. 若使用 payload，校验 JSON 类型和取值范围，并保证 cJSON 在所有返回路径正确释放。
9. 除非需求明确要求跨重启保存，否则按钮只修改运行时状态。
10. 完成后运行项目已有的清单校验或构建流程，并检查最终 diff 没有无关修改。

开始修改前先在内部确认：
- 这是业务级动作，还是确实需要框架级系统动作？
- 重复点击应该累计、幂等、切换，还是冷却/去重？
- 状态是否需要跨进程重启保存？
- 用什么现象验证按钮已经真正执行，而不只是成功入队？

能够从现有代码和上述需求确定时直接实施；只有缺少会实质改变业务行为的信息时才询问。

完成后说明：
- 修改了哪些文件；
- logic.json 中声明了什么按钮；
- handler 如何分支和修改状态；
- 动作是立即执行还是异步入队；
- 做了哪些验证以及应观察什么结果。
```

## 源码地图

| 文件 | 职责 |
|---|---|
| `vision_analysis/src/logic/modules/logic_xxx/logic.json` | 当前 logic 的按钮元数据源；打包后聚合给 Web |
| `vision_analysis/src/logic/modules/logic_button_demo/logic.cpp` | 最小自定义按钮示例：`+1` / `-1` 修改每通道数字并显示到画面 |
| `vision_analysis/src/logic/core/channel_logic.h` | `ChannelAction`、`ChannelActionResult`、handler 类型和注册宏 |
| `vision_analysis/src/logic/core/channel_logic.cpp` | 普通 logic 与 action handler 两套注册表 |
| `vision_analysis/src/control/channel_control.cpp` | Unix Socket 协议、系统动作、每通道队列和控制线程 |
| `vision_analysis/src/analyzer/channel_pipeline.cpp` | 每帧取队列、logic 名校验、执行 handler 后再执行逐帧 logic |
| `vision_analysis/src/main.cpp` | 控制服务初始化与退出清理 |
| `web_console/backend/services/process_manager.py` | 启动 App 时设置 `RK_CHANNEL_CONTROL_SOCKET` |
| `web_console/backend/routers/channel_control.py` | 动作清单 API、HTTP 到 Unix Socket 的桥接 |
| `web_console/frontend/src/api/client.ts` | action TypeScript 类型与 API 封装 |
| `web_console/frontend/src/pages/AppsPage.tsx` | 实时画面按钮渲染、确认、busy 状态和结果提示 |
| `vision_analysis/build.sh` | 校验并聚合 `src/logic/modules/*/logic.json`，生成 App 根目录 `logics.json` |

相关文档：

- `channelcontext-api.md`：handler 和逐帧 logic 可访问的 `ctx` 数据；
- `logic-naming-and-registration.md`：logic 名称与自注册机制；
- `adding-config-parameter.md`：需要持久化、可热重载的配置参数；
- `upload-and-wiring.md`：按钮触发告警/上报时的接线方式。
