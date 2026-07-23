# 告警上传微服务

本服务直接消费 C++ 写入 `alarm_store/<event_id>/manifest.json` 的统一事件发件箱，不使用 Redis。

## 模块

- `main.py`：读取配置、处理退出信号并启动消费者。
- `event_outbox.py`：扫描事件、解析 Dify 参数映射、维护每个 delivery 的状态；服务器使用固定 JSON，不发送算法参数。
- `dify_uploader.py`：上传图片或事件 MP4，并调用 Dify 工作流；视频分辨率取决于 Web 选择的原始源帧或通道显示帧模式。
- `config.yaml`：默认服务器、默认 Dify 连接和可复用 Profile。

每个投递独立重试。全部投递成功后立即删除事件目录；网络中断、远端拒绝或配置无效时保留记录，Web“待上报记录”页面可继续查看。SOP 正常结果使用仅 JSON 投递，不生成图片或视频。

## 配置

```yaml
dify:
  api_url: "http://dify.example.com"
  api_key: "app-..."
  timeout: 120

server:
  url: "http://server.example.com/api/alarm"
  timeout: 15

profiles:
  sop_dify:
    type: dify
    api_url: "http://dify.example.com"
    api_key: "app-..."
  alarm_server:
    type: server
    url: "http://server.example.com/api/alarm"
    token: "..."
```

画布中的 delivery 通过 `profile_id` 引用 Profile。地址和密钥不会进入通道配置。

## 运行

```bash
pip3 install -r requirements.txt
python3 main.py
```

`ALARM_STORE_DIR` 可覆盖默认发件箱目录。

## Dify 图片+视频联合分析测试

该测试直接复用生产环境的 `DifyUploader.upload_media_list()`，不会创建告警记录。Dify 工作流开始节点只需两个输入变量：

- `media_files`：`File list`，同时允许“图片、视频”，允许本地上传，最大数量至少为 2。
- `prompt`：文本类型，建议设置为必填。

测试使用独立配置 `tests/dify_video_test.yaml`，不会读取或修改生产环境的 `config.yaml`。先填写：

```yaml
dify:
  scheme: http
  host: 192.168.2.98
  port: 8015
  base_path: ""
  api_key: "app-你的工作流API密钥"
  timeout: 120
  user: "rk3588-dify-video-test"
workflow:
  media_variable: "media_files"
  prompt_variable: "prompt"
```

`media_variable` 和 `prompt_variable` 必须与 Dify 开始节点的变量标识完全一致。LLM 节点开启“视觉”后，视觉变量选择 `media_files`；输出节点把 `LLM.text` 保存到输出变量。

修改工作流后必须重新发布。测试程序会先读取 `/v1/parameters`，确认 `media_files` 是文件列表、同时允许图片和视频、允许 `local_file`、最大数量不少于 2，并检查图片和视频大小限制。

然后运行：

```bash
python3 tests/dify_video_test.py \
  --video /userdata/test.mp4 \
  --image /userdata/alarm.jpg \
  --prompt "请结合图片和视频分析异常行为"
```

脚本返回码为 `0` 且打印 `[PASS]`，表示文件上传和工作流执行均成功；随后 `[Dify回复]` 会打印 Dify 输出节点的全部 `outputs` 变量。返回码 `1` 表示 Dify 拒绝上传或工作流执行失败；返回码 `2` 表示本地参数或配置错误。

服务器图片接口继续发送 `base64Data` 和 `base64DataRaw`；Dify 图片及视频先调用 `/v1/files/upload`，再调用 `/v1/workflows/run`。
