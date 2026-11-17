# 在单个 C++ 文件里运行视频链路

公开入口是 [`<runtime.h>`](../../engine/include/runtime.h) 中的 `vision::VideoRuntime`。
正式 `vision_analysis` 和独立测试都调用它，采集、模型推理、跟踪、业务回调及退出清理共用实现。

最小多路取流示例位于 [`multi_stream/main.cpp`](../../engine/sdk_standalone_tests/multi_stream/main.cpp)。
它只包含公共头文件，每路首次取得原图时打印通道编号和尺寸，随后持续取流。
配置未启用模型、绘图、显示、RTSP 输出或事件。

## 编译与运行

```bash
engine/sdk_standalone_tests/build_cpp.sh engine/sdk_standalone_tests/multi_stream
engine/sdk_standalone_tests/multi_stream/multi_stream engine/sdk_standalone_tests/multi_stream/config.json
```

运行前，把 `config.json` 中的两个 `stream.url` 改成实际地址，并填写对应的 `h264` 或 `h265`。
也可以使用板端文件：`src_type` 填 `file`，`url` 填文件绝对路径。添加更多路时使用不同的通道 `id`。

独立程序只需要注册函数，无参数时不必另写参数目录：

```cpp
void receive_frame(ChannelContext *ctx)
{
    const auto *frame = ctx->source_frame();
    if (!frame || frame->empty())
        return;
    // 在这里处理当前通道的原图；图片在本次回调期间有效。
}
REGISTER_LOGIC(receive_frame);
```

配置中的 `logic` 填 `receive_frame`，`logic_parameters` 填 `{}`。
未知函数名、多余参数仍会被拒绝。有可调参数时，在首次解析配置前调用 `set_logic_catalog()` 描述参数；
显式提供目录后，函数必须在目录中有对应定义。正式业务继续使用构建时生成的目录。

示例设置 `max_fps: 5`，业务回调最多每秒处理约 5 张图片。处理来不及时保留最新图片，
适合实时分析，不保证每一张解码图片都调用业务函数。

## 运行和停止

```cpp
vision::VideoRuntimeOptions options;
options.config_path = "config.json";
options.install_signal_handlers = true;
vision::VideoRuntime runtime(options);
const int code = runtime.run();
if (code != 0)
    std::cerr << runtime.last_error() << '\n';
```

- `run()` 阻塞到结束，返回前完成线程回收和已提交事件的保存。启用本地显示时，从主线程调用。
- `request_stop()` 可从业务回调或其他线程调用，只提出停止请求；另一线程调用 `run()` 时，销毁对象前还要 join。
- `install_signal_handlers=true` 时可用 Ctrl+C 退出；返回后恢复原来的信号处理。
- 一个进程只允许一次启动尝试；停止后重新启动程序。提前调用 `request_stop()` 可取消尚未启动的运行。
- `is_running()` 表示 `run()` 尚未返回，连接、运行和清理阶段都可能为 true。

通道回调、命令回调或全局回调抛出 C++ 异常时，框架记录原因并停止整个运行时，
完成清理后返回 `7`。异常前已经提交的外部操作不会自动撤回。
其他返回码包括：`1` 初始化异常或 NPU 被占用，`5` 事件保存失败，`6` 重复启动，`-1`～`-4` 启动失败。

## 查看各路状态

运行中可以在另一线程查询；返回的是一份独立数据，不借用内部对象：

```cpp
for (const auto &channel : runtime.channel_status())
{
    std::cout << "通道 " << channel.channel_id
              << "，已收到 " << channel.received_frames << " 张图片\n";
    if (!channel.error.empty())
        std::cerr << channel.error << '\n';
}
```

| 状态 | 含义 |
| --- | --- |
| `Connecting` | 正在连接或重新连接，尚未收到当前源的图片 |
| `Streaming` | 已收到图片；暂停时保持此状态 |
| `Disconnected` | 断流或文件播放结束 |
| `Failed` | 采集器启动失败或本通道业务线程异常，原因在 `error` |
| `Stopped` | 运行时已完成清理 |

`width/height` 是最近原图的尺寸，`last_frame_age_ms` 是距最近收到图片的毫秒数；
尚未收到图片时分别为 `0/0` 和 `-1`。计数包含因 `max_fps` 限制而没有进入业务处理的图片。
收到恢复后的图片会清除断流错误。配置加载前列表为空；退出后保留最后一次状态快照，
正常通道转为 `Stopped`，失败通道及其原因保留。全局回调或启动错误通过 `last_error()` 查询。

## 资源与共享视频源

纯取流不申请 NPU 所有权。首次加载模型前申请锁，模型卸载并销毁后释放；
热更新重新启用模型时也必须重新取得锁。图片推理程序和视频程序不能同时占用 NPU。
仅临时关闭逐帧推理但仍保留已加载模型时，锁也会保留。

原来没有推理线程的通道要首次启用模型，仍需重启；现有推理通道可按原规则热更新模型，
取得 NPU 锁失败时保留原配置和线程。

相同地址、编码的 RTSP 通道可共用采集器；USB 还要求采集尺寸相同。
所有共享通道在采集器构造时一次确定，运行中不修改列表。文件通道有各自的播放和 seek 进度，
即使路径相同也不会共用采集器。通道业务状态仍然各自独立。
