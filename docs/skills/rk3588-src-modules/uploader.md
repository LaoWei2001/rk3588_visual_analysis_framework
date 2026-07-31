# 外部事件投递服务

上传不在 C++ `src/` 中实现。`service/upload/` 消费 `event_store`，通过适配器发送标准事件。

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
  "profile_id": "factory",
  "contract_id": "object_invade_det",
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
