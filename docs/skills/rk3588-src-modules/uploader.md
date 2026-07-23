# 上传服务边界（非当前 C++ 源码模块）

当前 `vision_analysis/src/` 中没有 `uploader/` 目录，也没有“写 Redis 队列”的上传实现。C++ 端负责在 `alarm_store/<event_id>/` 建立持久化事件发件箱；项目中的独立上传服务读取 `manifest.json`，再向业务服务器或 Dify 投递。

因此二次开发时应区分：

- 告警触发、字段、图片和投递清单：看 [alarm.md](alarm.md)。
- 报警前后视频：看 [recorder.md](recorder.md)。
- HTTP/Dify 请求、鉴权、重试和清理：在外部上传服务中修改，不要在通道 logic 里直接发送网络请求。

当前 `service/upload/event_outbox.py` 的协议边界为：

- 服务器仅支持图片，发送固定字段 `source/eventType/detResult/snapTime/endTime/base64Data/base64DataRaw/invadeFlag/eventId`；其中 `detResult` 当前固定为空对象，logic fields 不会自动进入服务器 JSON；
- Dify 支持图片或视频，媒体先上传，再按 delivery 的 `inputs` 把 `event.*`、`channel.*`、`logic.*` 映射到 workflow 输入；
- 每条 delivery 独立维护 `pending/uploading/delivered/retry/invalid`，只有全部为 `delivered` 才删除事件目录；
- `profile_id` 为空使用默认连接，非空时必须在 `services/upload/config.yaml.profiles` 中存在且类型匹配。

同一个事件可同时包含多条 delivery。上传服务按每条 delivery 的 `media/target` 分派：服务器图片走 `_send_server_image()`，Dify 图片和视频走 `_send_dify()`；这只是服务层发送实现不同，业务 logic 仍只调用一次统一 `report_alarm()/alarm_report()`。服务器图片读取同一事件的 `snapshot.jpg` 和 `raw.jpg`，Dify 图片读取 `snapshot.jpg`，Dify 视频读取 `clip.mp4`。完整教学流程见 `../rk3588-channel-logic/references/examples/logic_upload_teach.md`。

## C++ 与上传服务的契约

事件目录至少以 `manifest.json` 为状态真值。C++ 会把事件字段、媒体就绪情况和按 `report_policy` 展开的 `deliveries` 写入清单；图片工作线程生成图片，录像模块完成后回写视频就绪状态。上传服务应只处理媒体已就绪且尚未成功的 delivery，并把结果原子地写回清单。

排障顺序：

1. 确认 logic 的 `report_alarm()` 返回非空事件 id。
2. 检查对应 `manifest.json` 的 `fields`、`deliveries`、媒体状态和错误信息。
3. 检查 `raw.jpg`、`snapshot.jpg`、`clip.mp4` 是否按请求生成。
4. 最后检查上传服务日志、目标地址和鉴权。

不要重新引入旧文档中的 `alarm_uploader_enqueue`、`dify_uploader_enqueue`、`dify_queue` 或 Redis 链路；这些不是当前源码 API。
