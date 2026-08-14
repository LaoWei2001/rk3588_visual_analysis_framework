# 外部事件投递服务

上传不在 C++ `src/` 中实现。`service/upload/` 消费 `event_store`，通过适配器发送标准事件。

仓库 `service/upload/config.yaml` 和 `contracts/` 是打包默认值。Web 管理模式首次把它们迁移到
`/opt/ai_apps/.data/<App>/upload_config.yaml` 与 `contracts/`，并把事件放到同级 `event_store/`；
服务通过 `UPLOAD_DATA_DIR`、`EVENT_STORE_DIR` 读取这些持久路径。修改运行契约后要重启服务，
需要随代码分发时再同步回仓库默认值。

核心边界：

```text
EventOutboxForwarder
  - 读取三份状态文件
  - 等待所需媒体 ready
  - 维护重试状态
  - 按 contract_id 加载接口模板
  - 根据接口契约的 adapter 查注册表
        -> HttpJsonAdapter
        -> DifyWorkflowAdapter
        -> 新增 adapter
```

标准 delivery：

```json
{
  "profile_id": "server_22",
  "contract_id": "jnu_alarm_upload",
  "media": ["annotated_image", "raw_image"]
}
```

字段改名、增删、嵌套路径、常量和成功条件在 `service/upload/contracts/*.json` 中维护；
URL、Header、Token 在 Profile 中维护。只有交互步骤、签名算法或协议发生根本变化时才修改 adapter。

新增适配器只修改：

- `service/upload/adapters/<adapter>.py`
- `service/upload/adapters/registry.py`
- `service/upload/adapters/catalog.json`
- `service/upload/contracts/<interface>.json`
- 对应测试

不要修改 logic、C++ 事件核心或发件箱循环。

网络异常与可恢复 HTTP 状态使用无次数上限的指数退避（10 秒起步、最长 5 分钟）；明确不可恢复的
HTTP 4xx、契约错误或媒体生成失败才进入终态。长期断网能否完整补报还受 C++ 发件箱容量和磁盘
最低余量限制。
