# 通用事件投递服务

本服务消费 `event_store/<event_id>/`，负责投递状态、失败重试和适配器分派。它不参与视觉
推理，也不包含 SOP 等业务判断。

## 目录

```text
service/upload/
├── main.py
├── event_outbox.py
├── delivery_tool.py
├── contracts.py
├── contracts/
│   ├── object_invade_det.json
│   └── dify_*.json
├── config.yaml
└── adapters/
    ├── catalog.json
    ├── registry.py
    ├── mapping.py
    ├── http_json.py
    └── dify_workflow.py
```

- `event_outbox.py`：只维护 pending/uploading/retry/delivered/invalid/failed 状态。
- `registry.py`：`adapter ID -> Python 类` 的唯一分发表。
- `catalog.json`：Web 使用的适配器能力和 Profile 字段清单。
- `mapping.py`：稳定事件路径、类型转换、嵌套 target 和媒体转换。
- `contracts/*.json`：接口字段、固定值、媒体、请求方式和成功条件的唯一权威。
- `delivery_tool.py`：请求预览和测试发送，不修改发件箱状态。

## Profile

连接地址和密钥集中在 `config.yaml`：

```yaml
profiles:
  factory:
    adapter: http_json
    url: http://server.example.com/api/events
    timeout: 15
    headers:
      Authorization: Bearer replace-me

  inspection:
    adapter: dify_workflow
    api_url: http://dify.example.com
    api_key: app-replace-me
    timeout: 120
```

接口契约的 `adapter` 必须与所选 Profile 的 `adapter` 一致。不存在隐式默认连接。

## 接口模板

字段 mapping 集中保存在 `contracts/*.json`，而不是复制进每个通道 delivery。Web 上报节点
提供接口模板编辑器：它读取当前 logic 的 `report_fields`，并让开发者从系统事件、算法变量、
媒体或固定值中选择 source，再填写远端 target。保存后生成或更新模板文件。
`object_invade_det.json` 已完整声明 `source=JNU`、`eventType=4005`、两张图片和成功条件。
正常运行时 Web 只选择 Profile 与接口模板。delivery 保存：

```json
{
  "profile_id": "factory",
  "contract_id": "object_invade_det",
  "media": ["annotated_image", "raw_image"]
}
```

adapter、mapping、请求方式和成功条件都不复制到 delivery；`media` 是供 C++ 建立媒体任务的
必要快照。
Python 每次投递都会按 `contract_id` 重新加载权威模板，因此普通字段或成功条件变化只修改一份模板并重启
投递服务。若模板改变了所需媒体，需在 Web 重新选择该模板并保存配置，使 C++ 获得新的媒体快照。

Web 修改的是已安装 App 的 `services/upload/contracts/`。希望下次重新打包仍保留该模板时，
应把确认后的 JSON 同步回仓库 `service/upload/contracts/`；应用包重装会以包内文件为准。

## 模板字段来源

标准 source：

- `event.id/type/message/trigger_unix_ms/...`
- `source.channel_id`
- `source.parameters.<key>`
- `fields.<logic自定义字段>`
- `media.annotated_image/raw_image/video`
- `event`、`source`、`fields` 整体对象
- `constant` 配合映射的 `value`

通用转换：

- `string/number/boolean/json`：`type`
- `json_string`：对象序列化为 JSON 字符串
- `base64`、`data_url`：媒体转入 HTTP JSON
- `file`：HTTP multipart 文件或 Dify 文件上传

HTTP 成功条件由模板定义，既可只判断状态码，也可判断响应 JSON 路径。Dify 适配器会先上传 `file`
映射，再将文件 ID 和其他工作流输入一起提交。

## 增加接口或平台

同一种 HTTP/Dify 协议增加接口时，只新增一个 `contracts/<name>.json`，不修改 adapter。

只有出现新的交互协议或签名算法时：

1. 新建 `adapters/<name>.py`，实现 `preview()` 和 `send()`；
2. 在 `registry.py` 注册 ID；
3. 在 `catalog.json` 声明连接字段；
4. 新增接口模板和单元测试。

不需要修改 C++、logic、事件 schema、发件箱循环或 Web 表单组件。

## 验证

```bash
python3 -m unittest discover -s service/upload/tests -p 'test_event_store.py' -v

# 真实 Dify 视频联调
python3 service/upload/tests/dify_video_test.py \
  --video /path/to/clip.mp4 \
  --prompt "请分析视频"
```

运行时默认目录为 App 根目录的 `event_store/`，可用 `EVENT_STORE_DIR` 覆盖。
