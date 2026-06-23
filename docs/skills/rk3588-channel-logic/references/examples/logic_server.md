# logic_server — 服务器告警上报（按间隔限频）

- **上报**：server（`alarm_uploader_enqueue`）
- **可调参数**：`report_interval_sec`（int，上报间隔秒，默认 5；与 logic_dify 共用）
- **用到的能力**：方案2 每通道地址、跨帧状态限频、最简上报骨架

## 做什么
"有检测结果就上报到 HTTP 服务器"，并按 `report_interval_sec` 限频。一旦本帧有任何检测结果、且距上次上报已达间隔，就把当前帧打包、带上本通道的 `server_url` 落地到本地发件箱（`alarm_store/`，由 Python 上报服务补传到 HTTP 服务器），告警类型 `"intrusion"`，再记下本次上报时间。首次（`last_upload_ms==0`）立即上报，之后按间隔冷却。间隔每帧从 `ctx->config->report_interval_sec` 现读，支持网页热重载。

## 完整实现
```cpp
struct ServerState {
    uint64_t last_upload_ms = 0;
};

static void logic_server(ChannelContext *ctx)
{
    if (!ctx || !ctx->results || ctx->results->empty())
        return;
    if (!ctx->frame)
        return;

    if (!*ctx->state) *ctx->state = std::make_shared<ServerState>();
    auto &s = *std::static_pointer_cast<ServerState>(*ctx->state);

    // 上报间隔(秒)→ms：每帧现读以支持热重载；首次立即上报，之后按间隔冷却
    const uint64_t interval_ms =
        (uint64_t)std::max(1, ctx->config ? ctx->config->report_interval_sec : 5) * 1000ULL;
    if (ctx->timestamp_ms - s.last_upload_ms < interval_ms)
        return;

    alarm_uploader_enqueue(*ctx->frame, *ctx->frame, ctx->chnId, "intrusion",
                           ctx->config->report_enable,   // 连了「上报配置」节点才真正发
                           ctx->config ? ctx->config->server_url.c_str() : nullptr);
    s.last_upload_ms = ctx->timestamp_ms;
}
```

## 接线
- 文件 / 注册：独立文件 `src/logic/logic_server.cpp`（顶部 `#include "logic_common.h"`），文件末尾自注册 `REGISTER_LOGIC("logic_server", logic_server);`。
- logics.json：`{ "name": "logic_server", "label": "服务器告警上报", "report": "server", "params": [ { "key": "report_interval_sec", "type": "int", "label": "上报间隔(秒)", "default": 5, "min": 1, "max": 3600 } ] }`
- `report_interval_sec` 字段已在 `config.h` 的 `ChannelConfig` + `config_init.cpp` 的 `REG_C` 注册（logic_server / logic_dify 共用）。
- 用户需在网页给该通道连一个「上报配置」节点并填 HTTP 地址（写进该通道 `config.json` 的 `server_url`）。

## 复用提示
"检测到 X 就上报服务器"的最简骨架（已自带间隔限频）。在此基础上加：① 类别过滤 `r.label != "person"` ② ROI 过滤。若要"持续 N 秒才报警、只发一次、条件消失复位"，用 `logic_hook` 的闩锁 + 双向计时范式。
