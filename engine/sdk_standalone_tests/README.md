# SDK 独立调用测试

本目录验证公共 SDK 接口能否脱离主程序和 Web，由独立 C/C++ 程序完成初始化、调用及资源释放。
包含独立测试源码、`build_cpp.sh` 编译脚本及配套 `CMakeLists.txt`；自动化回归测试位于 `engine/tests/`。
在 RK3588 板端进入此目录，编译时只需要指定一个测试文件夹：

```bash
cd /userdata/rk3588_visual_analysis_framework/engine/sdk_standalone_tests

./build_cpp.sh model_inspection
./build_cpp.sh image_inference
./build_cpp.sh drawing
./build_cpp.sh multi_stream
./build_cpp.sh analysis
./build_cpp.sh network_rtt
```

| 测试程序 | 用途 |
| --- | --- |
| `model_inspection/main.cpp` | 读取 RKNN 元数据，检查模型类型与张量结构 |
| `image_inference/main.cpp` | 批量图片推理与标注绘图 |
| `drawing/main.cpp` | 在空白图片上测试矩形、圆、线、中文和多边形绘制 |
| `multi_stream/main.cpp` | 使用正式 VideoRuntime 读取多路视频，在回调里取得每路原图 |
| `analysis/main.cpp` | 用人工检测结果测试跟踪、ROI、业务回调、事件与绘图 |
| `network_rtt/main.cpp` | 直接调用 SDK 测量到摄像头 IP 的 ICMP 网络往返延迟 |

程序直接输出到 **所选文件夹内，文件名与文件夹名一致**，公共构建入口统一处理 SDK 的头文件路径和库链接。
例如 `./build_cpp.sh model_inspection` 会生成 `model_inspection/model_inspection`。
`build/` 只保存编译缓存和中间文件。

## 运行

仍在本目录运行以下命令；将 `/path/to/` 替换为板端实际文件路径：

```bash
./model_inspection/model_inspection /path/to/model.rknn yolov8_det

./image_inference/image_inference yolov8_det /path/to/model.rknn /path/to/labels.txt /path/to/image.jpg

./drawing/drawing

./multi_stream/multi_stream multi_stream/config.json

./analysis/analysis

./network_rtt/network_rtt 192.168.1.64
```

图片示例在当前工作目录输出 `result-0.jpg` 等标注图，不需要标签的模型将标签参数填为 `-`。
网络测试只需摄像头 IP，不启动视频和模型；成功输出毫秒数，失败输出原因。
画面显示“板端—摄像头往返”：只表示这两个设备之间的通信往返时间，
不包含视频编解码、推理、板端到浏览器的传输和播放缓冲，不能当作 Web 画面的总延迟。
接口在 `<network.h>`，自建 CMake 工程可只链接 `Vision::Network`。
摄像头不响应 ICMP 或宿主没有 ICMP 权限时，测量会失败，不代表 RTSP 视频一定不可用。
绘图示例无需模型和摄像头，在当前工作目录输出 `drawing-result.png`。
修改 `drawing/main.cpp` 中的 `test_drawing()` 后重新编译即可；坐标按 640×640 填写，
颜色顺序为蓝、绿、红。`LogicSession` 准备 `ctx` 并执行你的函数，
`render_overlays()` 把记录的图形画到图片上，`cv::imwrite()` 负责保存。
不要直接创建一个空的 `ChannelContext` 调用 `draw_rect()`，它还没有准备好存放绘图内容的位置。
若提示字体加载失败，可设置 `RK_OVERLAY_FONT=/实际路径/中文字体.ttf` 后重试。
多路取流示例先修改 `multi_stream/config.json` 中的两个 `stream.url`，替换为实际 RTSP 地址，
并按视频编码填写 `video_enc`（`h264` 或 `h265`）。每路收到图片后打印一次通道编号和尺寸，
随后持续接收，按 Ctrl+C 退出。需要更多路时复制通道配置并使用不同的 `id`。
源码只有一个 `main.cpp`；模型推理、绘图、显示、RTSP 输出和事件均未启用。
无参数的回调只需 `REGISTER_LOGIC(receive_frame)`，不必另写空参数目录。
配置中的 `max_fps: 5` 限制业务回调频率；处理不过来时保留最新图片，不保证逐帧调用。
业务示例无需模型，默认生成 `intrusion-debug.jpg`，也可以传入背景图和输出路径。
视频运行和状态查询见[视频运行说明](../../docs/sdk/video-runtime.md)。
图片推理和加载了模型的视频运行需要空闲的 NPU；纯取流不占用 NPU 锁。

## 编译自己的文件夹

一个文件夹对应一个可执行程序，其中只能有一个 `main()` 入口。可以包含多个 C/C++ 源文件、
头文件及子目录，例如：

```text
my_test/
├── main.cpp
├── logic.cpp
├── logic.h
├── include/
│   └── helper.h
└── src/
    └── helper.c
```

```bash
./build_cpp.sh /path/to/my_test
/path/to/my_test/my_test
```

脚本递归收集 `.c`、`.cc`、`.cpp`、`.cxx`、`.C`，C 文件使用 C 编译器，C++ 文件使用 C++ 编译器，
最后链接成一个程序。`.h`、`.hh`、`.hpp`、`.hxx` 会作为头文件依赖参与增量构建。
源文件夹和其 `include/` 自动加入头文件搜索路径；其他子目录的头文件可按相对路径包含，
例如 `#include "utils/helper.h"`。C 函数供 C++ 调用时，需要在头文件中使用 `extern "C"` 保护。

`build/`、`cmake-build-*`、`CMakeFiles/`、隐藏目录和当前构建目录会自动排除，避免把生成文件再次编译。
新增、删除源文件或修改头文件后，重新执行同一条命令即可。
传入的相对路径以当前工作目录为准，脚本可以从任意目录调用。

自建文件夹只需放源文件和头文件，按需包含 `engine/include/` 中的公共接口，例如：

```cpp
#include <model_inspection.h>
#include <inference.h>
```

头文件提供声明及类型，普通函数的实现来自 SDK 库。公共 CMake 入口统一链接 `Vision::Runtime`，
它包含模型检查、图片推理、分析及视频运行能力的传递依赖；每个文件夹都使用同一规则，无需额外配置文件。
首次构建会编译完整 SDK 实现，后续增量复用；实际调用接口时才创建模型会话或启动视频运行。

每个文件夹使用自己的 `build/` 缓存，首次构建编译所需 SDK 库，后续在该目录增量复用。
不同文件夹的构建互不覆盖，同名文件夹位于不同路径时也各自输出。
设置 `VISION_BUILD_JOBS` 可调整编译并行数，默认是 `4`。
如设置 `VISION_SDK_BUILD_DIR`，只改变缓存目录，可执行文件仍生成在所选源文件夹内；并行构建时不要共用这个缓存目录。

完整 SDK 接入说明见 [独立 C++ 测试](../../docs/sdk/standalone-cpp.md)。
