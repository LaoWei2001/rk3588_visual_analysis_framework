# 上报机制（方案2）与接线参考

## 一、上报：地址跟着每条告警走，不在逻辑里硬编码

项目的上报是**两段式**：C++ 逻辑只把告警 + **本通道地址**非阻塞投递（server 告警落本地发件箱 `alarm_store/`、dify 走 Redis `dify_queue`）；真正发到哪台服务器由 Python 上报服务按记录/消息里的地址决定。所以**不同通道可发不同服务器**，而你在逻辑里只需把 `ctx->config` 里的地址透传给入队函数。

签名见 `rk3588_yolo/src/uploader/alarm_uploader.h`：

```cpp
// HTTP 服务器上报：img_draw=带框图, img_raw=原图(可相同), camera_id=ctx->chnId,
// alarm_type=告警类型字符串,
// report_enable=是否真的发(画布连了「上报配置」节点才 true; false 时函数内部直接跳过),
// server_url=本通道地址(nullptr/空=用上报服务默认值)
int alarm_uploader_enqueue(const cv::Mat &img_draw, const cv::Mat &img_raw,
                           int camera_id, const char *alarm_type, bool report_enable,
                           const char *server_url = nullptr);

// Dify 上报：prompt=提示词, event_id=事件唯一标识,
// report_enable=同上, dify_api_url/key=本通道 Dify 地址与密钥
int dify_uploader_enqueue(const cv::Mat &img, const char *prompt, const char *event_id,
                          bool report_enable,
                          const char *dify_api_url = nullptr, const char *dify_api_key = nullptr);
```

### 上报开关：画布连了「上报配置」节点才上报（必做）
是否上报由**画布上有没有连「上报配置」节点**决定，不由 logic 自己说了算——这样这个节点才是真正的开关，而非摆设。节点存在 → `graphToConfig` 写 `config.json` 该通道 `report_enable: true`。**调上报函数时直接把 `ctx->config->report_enable` 作为 `report_enable` 参数传进去即可**：函数内部见它为 false 会直接跳过，不用你再写 `if`。（`ctx->config` 在 logic 运行期间恒非空——它是 `&channels[chnId]`，可放心解引用。）

### server 上报标准写法
```cpp
const char *url = (ctx->config && !ctx->config->server_url.empty())
                      ? ctx->config->server_url.c_str() : nullptr;
// 第 5 个参数 ctx->config->report_enable: 没连「上报配置」节点就为 false → 内部不发
alarm_uploader_enqueue(*ctx->frame, *ctx->frame, ctx->chnId, "intrusion",
                       ctx->config->report_enable, url);
```

### dify 上报标准写法
```cpp
const char *prompt = (ctx->config && !ctx->config->dify_prompt.empty())
                         ? ctx->config->dify_prompt.c_str() : "请分析画面";
const char *durl = (ctx->config && !ctx->config->dify_api_url.empty())
                       ? ctx->config->dify_api_url.c_str() : nullptr;
const char *dkey = (ctx->config && !ctx->config->dify_api_key.empty())
                       ? ctx->config->dify_api_key.c_str() : nullptr;
char event_id[64];
snprintf(event_id, sizeof(event_id), "ch%d_f%lld_t%llu",
         ctx->chnId, (long long)ctx->frame_id, (unsigned long long)ctx->timestamp_ms);
dify_uploader_enqueue(*ctx->frame, prompt, event_id, ctx->config->report_enable, durl, dkey);
```

### 上报图带框
若要上报的图上带检测框/标注，先克隆帧、`render_overlays` 渲染，再上报：
```cpp
cv::Mat upload_img = ctx->frame->clone();
RenderParams rp = ctx->render_params();
rp.target_mask = DrawCommand::UPLOAD;
render_overlays(upload_img, rp);
alarm_uploader_enqueue(upload_img, *ctx->frame, ctx->chnId, "xxx", ctx->config->report_enable, url);
```

### 限频（必做）
上报很贵，别每帧发。用 `ctx->timestamp_ms` 卡间隔，或用闩锁（一次告警只发一次，复位后才能再发）。闩锁+双向计时的范例见 `examples/logic_hook.md`；按 track_id 去重的范例见 `examples/logic_dify_person_verify.md`。

### `ctx->config` 里现成的上报相关字段
| 字段 | 类型 | 用途 |
|------|------|------|
| `report_enable` | bool | **上报总开关**：画布连了「上报配置」节点才为 true。直接作为上报函数的 `report_enable` 参数传入（`ctx->config->report_enable`） |
| `server_url` | string | HTTP 上报地址（每通道，网页「上报配置」节点填） |
| `dify_api_url` | string | Dify 地址 |
| `dify_api_key` | string | Dify 密钥 |
| `dify_prompt` | string | Dify 提示词 |

## 二、注册：让程序认得这个 logic

新建独立文件 `rk3588_yolo/src/logic/logic_xxx.cpp`（顶部 `#include "logic_common.h"`、实现 `static void logic_xxx(ChannelContext *ctx)`），在文件**末尾**写一行自注册（`main()` 前自动登记，**无需改动 `channel_logic.cpp`**）：
```cpp
REGISTER_LOGIC("logic_xxx", logic_xxx);
```
> `src/logic` 下的 `.cpp` 由 CMake（`aux_source_directory`）自动收集编译；项目 `build.sh` 每次全新构建，新增/删除文件自动包含。删除一个逻辑＝删除对应 `logic_xxx.cpp` 文件。

## 三、声明到 logics.json：让网页能选、能渲染参数

`rk3588_yolo/src/logic/logics.json` 的 `channel_logics` 数组加一条：
```json
{ "name": "logic_xxx", "label": "中文显示名", "report": "server", "params": [] }
```
- `name`：必须与 `REGISTER_LOGIC` 里的名字、config.json 里 `"logic"` 的值完全一致。
- `label`：网页下拉里显示的中文名。
- `report`：`"server"` / `"dify"` / 不写。**仅作标注意图**（说明这个 logic 设计上会上报），网页据它在节点上把上报类型默认成 server/dify。**它不再自动带出节点、也不决定是否上报**。
- `params`：可调参数清单（见下）。无参数就 `[]`。

> **「上报配置」节点 = 上报的唯一开关**（本会话重构）：
> - **连了**「上报配置」节点 → 保存时 `graphToConfig` 写该通道 `report_enable: true`（并按节点的 `report_type` 写地址）→ C++ `ctx->config->report_enable` 为真 → logic 才上报。
> - **没连** → `report_enable: false` → `ctx->config->report_enable` 为假 → 即使 logic 里有上报代码也不发。这样节点是真开关，不是摆设。
> - 与 logic 是否在 logics.json 声明 `report` **完全无关**：声明了也不会"没连也报"，没声明也能手动连节点来报。
> - 刷新/重载：`configToGraph` 据通道里的 `report_enable` 把节点重建出来（老配置无此字段时，回退看通道有没有 `server_url`/`dify_*` 上报字段）。
> - **内置上报类 logic（server/dify/hook/fall_detect/dify_person_verify）都已按此 gate**；你**自己写的 logic 把 `ctx->config->report_enable` 作为上报函数的 `report_enable` 参数传入即可**（false 时函数内部跳过），否则不受节点控制。
> - 迁移提醒：旧 `config.json` 没有 `report_enable` 字段，C++ 默认 false → 升级后**需在网页重新打开配置并保存一次**（旧的上报通道会因带有 `server_url` 等字段而自动重建出节点），节点写入 `report_enable: true` 后才恢复上报。

## 四、可调参数：三处对齐，缺一不可

若逻辑需要用户能在网页上改的数值（半径、停留秒数、阈值…），`param.key` 必须三处一致：
**① config.h 的 ChannelConfig 字段 == ② config_init.cpp 的 REG_C 键 == ③ logics.json 的 param.key == 逻辑里 `ctx->config->key` 读的键。**

1. `rk3588_yolo/src/config/config.h`，`ChannelConfig` 加字段（给默认值）：
   ```cpp
   int dwell_sec = 3;        // 停留报警秒数
   ```
2. `rk3588_yolo/src/config/config_init.cpp`，加注册（类型选 `INT`/`FLOAT`/`BOOL`/`STRING`/`STRING_ARRAY`）：
   ```cpp
   REG_C("dwell_sec", INT, dwell_sec);
   ```
3. `logics.json` 该 logic 的 `params` 加一条：
   ```json
   { "key": "dwell_sec", "type": "int", "label": "停留秒数", "default": 3, "min": 1, "max": 60,
     "help": "目标在 ROI 内连续停留超过此秒数即报警" }
   ```
   `type` 取值：`int` / `float` / `string` / `bool` / `enum`（配 `options`）/ `text`（多行）。

4. 逻辑里读：`int dwell = ctx->config ? ctx->config->dwell_sec : 3;`

> logics.json 顶部 `_comment` 就是这条规则的权威说明，照它做即可。

## 五、config.json 接线（给开发者的说明，不要你直接改）
让目标通道用上这个 logic：网页画布该通道的「逻辑函数」节点选 `logic_xxx`，参数在节点上调；若上报，连一个「上报配置」节点并填 server/dify 地址。等价于手改该 App 的 `config.json` 对应通道：`"logic": "logic_xxx"`，并按需加 `"server_url"` / `"dify_api_url"` 等。
