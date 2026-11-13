# 运行时控制接口

本目录负责 Web/外部设备与 C++ 主程序之间的本地控制接口。业务模块使用
`engine/include/actions.h` 中的公开类型，不依赖这里的私有实现。

| 文件 | 职责 |
| --- | --- |
| `logic_control.*` | 通道/全局 Action 入队，系统级 `infer_toggle` |
| `file_playback_control.*` | 查询本地文件播放位置、时长和执行 seek |
| `remote_dataset.*` | 数据集控制 Socket、同帧采集观察器、手动采集和后台编码 |
| `socket_shutdown.h` | 关闭时中断在途连接的阻塞读写，协调 Socket 的释放所有权 |

## 动作执行

```text
Web / 外部控制器 → FastAPI → Unix Socket
  ├─ 系统动作 infer_toggle：修改通道运行时推理开关
  └─ 业务动作：入队 → 下一次业务帧 → LogicAction handler → 正常 Logic 回调
```

通道 ID 是 `config.channels[].id`，不是配置数组下标。业务动作携带入队时的 Logic 名称；
热切换后，旧模块的待处理动作会被丢弃。

公开类型是 `LogicAction`、`LogicActionResult`，使用 `REGISTER_LOGIC_ACTION` 注册处理器。
动作参数、队列边界、HTTP 路径与鉴权方式以[通道 Action 文档](../../../../docs/skills/rk3588-channel-logic/references/actions.md)
和当前实现为准。`accepted` 只表示请求入队，业务执行结果写入应用日志。

## 文件播放与数据采集

文件播放控制使用独立接口，不经过 Logic 动作队列。多个通道共享同一个文件采集器时，
接口返回一条时间轴及其全部通道 ID；seek 会重置相关通道状态，并丢弃跳转前的在途旧结果。

数据集采集接口独立于通道 Logic。采集规则位于 `src/dataset/`，Web 端服务在
`web_console/backend/services/dataset_manager.py`。完整职责见[采集架构](../../../../docs/architecture/dataset-collection.md)。

## 退出顺序

各控制服务停止时先拒绝新工作并中断在途 Socket I/O，再等待所属线程结束。
`ControlConnection::cancel()` 只执行 `shutdown()`；服务线程负责最终 `close()`，
避免其他线程提前关闭后文件描述符被复用。

总退出顺序由 [`application/main.cpp`](../main.cpp) 编排。更改控制线程、阻塞操作或
后台编码时，同时检查其 `deinit()` 与调用顺序，确保相关对象在线程结束后再销毁。
