# 应用级事件投递服务

投递服务消费当前程序的 `event_store`，使用画布已经绑定的连接与版本化契约发送图片、视频和
业务字段。视觉 Logic 不访问网络，Adapter 不包含业务字段名。

## 能力分层

```text
Logic                         产生事件、fields 和媒体需求
Report contract template      远端字段、请求格式、成功条件
Delivery connection           地址、认证、超时
Canvas delivery binding       connection_id + contract_id + contract_revision
Adapter                       HTTP / Dify 协议实现
```

## 模板来源

- `src/logic/modules/<logic>/report_templates/`：Logic 随附模板；必须在同目录 `logic.json` 的
  `report_templates` 中声明。
- `vision_analysis/report_templates/`：当前程序包或客户项目专用模板。

系统不提供公共默认模板。新增上报功能时，Logic 专用契约放在对应 Logic 目录；跨 Logic 但只属于
当前应用的客户接口放在 `vision_analysis/report_templates/`。Adapter 只实现协议，不携带默认业务契约。

打包时 `scripts/generate_report_templates.py` 校验模板归属、事件类型、算法字段、媒体和 Adapter
能力，并把结果聚合到程序包只读目录 `report_templates/`。

设备上通过 Web 创建或修改的契约保存在：

```text
/opt/ai_apps/.data/<app>/report_contracts/
```

Logic/应用模板和应用自定义契约在读取时合并；自定义契约可覆盖同 ID 包模板。不存在复制包内 contracts
到 `.data` 的步骤，所以程序升级能立即带来新增模板，又不会覆盖应用自定义契约。

## 连接

连接保存在当前应用的数据目录：

```text
/opt/ai_apps/.data/<app>/connections.yaml
```

HTTP 连接只保存 `base_url`、认证 Header 和超时；接口 path、编码和字段位置属于契约。Dify 连接
保存 API 基础地址、工作流 App API Key 和超时。连接不进入程序包。

## 契约 revision

契约内容使用 SHA-256 形成 revision。画布 delivery 必须保存：

```json
{
  "connection_id": "production",
  "contract_id": "logic_periodic_snapshot.http",
  "contract_revision": "...",
  "media": ["annotated_image"]
}
```

投递服务把每个使用过的契约归档到 `.data/<app>/contract_revisions/`。契约被修改后，已经进入
发件箱的事件继续使用原 revision；新画布绑定使用新 revision。

## HTTP 请求模型

HTTP 契约的 `request` 声明 `method`、`path` 和 `encoding=json|form|multipart`。每条 mapping
通过 `location=body|query|form|header|file` 声明目标位置。`file` 位置必须使用 `transform=file`；
Multipart 同时携带 JSON body 时，由契约的 `request.json_part` 明确 JSON Part 名称，Adapter
不写死服务器字段。

## 结果记录

每次远端请求结果写入 `.data/<app>/delivery_history/`，包括连接、契约 revision、HTTP 状态、
重试次数和远端响应。Web“事件记录 → 远端回复历史”可以查看普通服务器响应或 Dify Outputs。

连接和契约在每次处理事件前重新加载；原子保存失败时不会生成半份运行配置。
