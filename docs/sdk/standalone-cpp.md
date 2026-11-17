# 用独立 C/C++ 程序测试框架能力

公共 API 在 `engine/include/`，实现按职责编译成库。正式程序和外部测试共享实现：
主程序不再单独编译一套推理、跟踪、查询或事件代码。

## SDK 的实现组件

下表用于说明 SDK 分工，以及自有 CMake 工程的链接方式。
本项目的 `engine/sdk_standalone_tests/build_cpp.sh` 统一链接 `Vision::Runtime` 提供完整能力，无需逐目录选择组件。

| 组件 | 可独立测试的能力 | 输入 |
| --- | --- | --- |
| `Vision::ModelInspection` | RKNN 输入输出信息、模型类型与张量兼容性检查 | 模型文件或人工构造的元数据 |
| `Vision::Inference` | 单帧多模型推理、检测/姿态/分割/深度及绘图 | BGR 图片、模型配置 |
| `Vision::Network` | 测量到摄像头的 ICMP 网络往返延迟 | IPv4/IPv6 地址 |
| `Vision::Analysis` | 上述能力，以及 SORT/ByteTrack、ROI/目标查询、业务回调、参数、跨帧状态、变量、动作、事件捕获、数据集规则 | 图片或人工检测结果、时间戳 |
| `Vision::Runtime` | 上述能力，以及真实视频采集、线程调度、跨通道全局逻辑、控制服务、RTSP、实际事件图片/录像/数据集采集、GPIO | 独立 JSON 视频配置 |

`Vision::SDK` 只提供公共头文件约束，用于编译业务模块；独立可执行文件还需链接对应实现组件。
链接库本身不启动 NPU、视频、GUI 或后台线程；`inspect_model()` 临时加载模型读取信息，
`ImageInferenceSession` 构造时持久加载模型用于推理。
GPIO 接口会操作设备，独立程序链接 Runtime 后也可直接调用 `gpio.h` 中的公开函数。

## 一个命令构建

在 RK3588 项目根目录运行：

```bash
engine/sdk_standalone_tests/build_cpp.sh engine/sdk_standalone_tests/analysis
engine/sdk_standalone_tests/analysis/analysis

engine/sdk_standalone_tests/build_cpp.sh engine/sdk_standalone_tests/multi_stream
engine/sdk_standalone_tests/multi_stream/multi_stream engine/sdk_standalone_tests/multi_stream/config.json
```

参数只有源文件夹，可执行文件直接生成在该文件夹内，文件名与文件夹名一致；`build/` 只保存编译缓存和中间文件。
一个文件夹对应一个程序，可以放多个 `.c`、`.cc`、`.cpp`、`.cxx`、`.C` 和头文件，并包含子目录；
只保留一个 `main()` 入口。源文件夹和其 `include/` 自动加入头文件搜索路径，构建目录会被排除。
换成你自己的源文件夹即可；源码按需包含公共 SDK 头文件，公共构建入口统一提供头文件路径并链接完整 SDK。
头文件提供函数声明和类型，普通函数的实现来自库，链接由脚本负责，无需逐目录配置。
编译脚本、配套 CMake 和使用说明统一位于 [`engine/sdk_standalone_tests/`](../../engine/sdk_standalone_tests/README.md)。
第一次构建编译完整 SDK 库，之后复用该源文件夹下 `build/` 的对象文件；不需要复制底层源码、包含
`engine/src` 私有头，或创建 `projects/` 模块。
新增、删除源文件或修改头文件后，重新执行同一条命令即可。
`VISION_BUILD_JOBS` 控制构建并行数；可选的 `VISION_SDK_BUILD_DIR` 只覆盖缓存位置，可执行文件始终位于源文件夹内。
SDK 库保持 Release 优化，测试 CPP 保留 assert 断言。
不同源文件夹默认各有独立缓存；手动覆盖目录时，并行构建应使用不同目录。
输出程序使用当前设备和仓库依赖，跨设备部署需一并携带对应运行库。

自有 CMake 工程可直接：

```cmake
set(VISION_BUILD_APPLICATION OFF CACHE BOOL "" FORCE)
add_subdirectory(/absolute/path/to/framework/engine vision-engine EXCLUDE_FROM_ALL)
add_executable(my_test test.cpp)
target_link_libraries(my_test PRIVATE Vision::Analysis)
```

静态库默认沿用工具链的可执行代码生成方式，不强制 PIC；若要嵌入自己的共享库，调用者可设置
`CMAKE_POSITION_INDEPENDENT_CODE=ON`。

构建 SDK 不读取业务 `projects/` 清单。主程序的正常构建方式保持为
`cmake -S engine -B build/engine`，默认同时构建业务应用。

## 独立检查模型

```bash
engine/sdk_standalone_tests/build_cpp.sh engine/sdk_standalone_tests/model_inspection
engine/sdk_standalone_tests/model_inspection/model_inspection /absolute/path/to/model.rknn yolov8_det
```

接口、错误处理和 Web 画布的完整调用链见[模型检查](model-inspection.md)。
例子只有标准 C++ 和 `<model_inspection.h>`，不需要 Python、Web 服务或程序包。

## 图片或人工结果测试真实业务回调

完整示例见 [analysis/main.cpp](../../engine/sdk_standalone_tests/analysis/main.cpp)。它用同一个通道回调完成
查询、ROI、跟踪、两秒驻留、变量发布、事件及绘图。时间从 0 推进到 1000、2000 毫秒，
直接检查报警行为，无需等待真实视频播放。

```cpp
#include <logic_session.h>

void test_logic(ChannelContext *ctx) {
    TargetQuery query;
    query.labels = {"person"};
    ctx->publish_int("count", ctx->query_targets(query).count);
}

int main() {
    vision::LogicSessionOptions options;
    options.logic = test_logic;
    vision::LogicSession session(options);
    std::vector<AlgoResult> targets(1);
    targets[0].label = "person";
    targets[0].score = 0.95f;
    targets[0].box = {100, 100, 50, 120};
    vision::LogicFrame frame;
    frame.results = &targets;
    frame.timestamp_ms = 1000;
    session.run(frame);
    return session.outputs().values().empty();
}
```

`LogicSession` 消费调用方给出的结果，不重复执行模型加载或模型阈值筛选。真实图片结果在
`ImageInferenceSession` 中按每个模型的配置完成筛选；人工结果则用于主动构造边界测试。

检测结果和 ROI 使用 **640×640 业务坐标**。原始 BGR 图片尺寸不限，`business_frame()` 在首次
请求时生成业务图。`ImageInferenceSession` 的结果已经映射到此坐标系，可直接接入：

```cpp
auto prediction = inference.infer(source);
vision::LogicFrame frame;
frame.source = &source;
frame.results = &prediction.results;
frame.inference_valid = prediction.succeeded();
frame.timestamp_ms = now_ms;
session.run(frame);
```

结果、图片和自定义 ROI 都是借用，必须存活到下一次 `run()` 或 session 销毁；不要传临时对象
内部的指针。每一路创建自己的会话和跟踪器。每帧输出/绘图/捕获事件清空，业务 state 跨帧保留。
`set_parameters()` 使用正式程序相同的 Schema 校验和重置策略，失败不会覆盖旧参数。
有命名参数时，在首次解析前用 `set_logic_catalog()` 注册与正式模块相同的 Schema；无参数
回调可以直接用默认空逻辑名。原来的自动嵌入依赖已移至应用入口，没有保留第二套参数解析器。

## 绘图与事件的边界

`session.drawing()` 是本帧业务绘图命令；`vision::render_overlays()` 可把检测结果、ROI、绘图
统一画到自己提供的 BGR 图片。`OverlayOptions` 传入业务坐标宽高和借用的数据列表。
正的 `result_frame_id` 表示该版本结果像素不变，允许缓存；不管理版本时保持 0。

离线会话默认捕获 `report_event()` 的请求和提交时绘图，供断言使用，**不持久化、不录像、不联网**。
需要自己的事件处理时传入 `EventSink`，对象应活过会话；不需要替换或重定义框架函数。
实际事件落盘、前后录像、RTSP 和多通道全局逻辑，通过
[VideoRuntime 示例](video-runtime.md) 使用正式链路。其运行约束、信号和退出返回码见该文档。

## 实现分工

- `Inference.cmake`：推理和图像渲染；`Analysis.cmake`：逐帧业务能力；`Engine.cmake`：真实运行时。
- `Application.cmake`：正式可执行入口与业务对象；`Business.cmake`：业务清单生成与注册。
- `src/logic/`：通道上下文集中在 `channel_context.cpp`，目标和 ROI 查询集中在 `target_query.cpp`；绘图提交、注册、参数定义、会话与全局逻辑分别组织。
- `src/events/`：公共提交、策略、媒体取帧、队列工作线程、存储分别实现。
- `src/media/rtsp/`：生命周期、帧供给、预览、DMA 分别实现。
- `src/application/runtime/video_runtime_*.cpp`：初始化、运行/停止、进程支持、资源回收。

移动为公共头文件的旧私有头已删除，所有现有调用方都迁移到新入口，不提供重复兼容实现。

板端实测与性能数据见 [2026-11-16 验证记录](verification-20261116.md)。
