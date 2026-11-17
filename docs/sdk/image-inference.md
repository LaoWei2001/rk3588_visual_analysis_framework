# 图片推理与公共单帧接口

图片调试和视频通道共用模型工厂、单帧执行器、ROI 处理、坐标映射、最终筛选和结果绘制。
图片任务输入 `CV_8UC3` BGR 图像，输出现有的 `AlgoResult`；可以一次配置多个不同模型，
每个模型保留自己的 ID、输入尺寸、标签、置信度、NMS、类别过滤和 NPU 核心配置。

## 在 Web 控制台使用

1. 打开侧边栏的「图片推理」。模型来自已安装的程序包，无需启动对应的视频程序。
2. 添加一个或多个模型，先选择「程序包」，再选择该程序包 `assets/` 中的 `.rknn` 模型和 `.txt` 标签。
   每个模型可使用不同程序包；切换程序包会清空之前选择的模型文件和标签。
   选择类型、模型文件后自动检查，显示输入尺寸与布局；检查中或不匹配时不能开始推理。
   模型文件下拉菜单末尾的「导入模型…」可上传电脑上的 RKNN 到当前程序包 `assets/`，完成后自动选中并检查。
   同名文件自动另存，保留原文件；标签通过程序包的资源管理维护。
3. 选择图片来源：「电脑上传」支持单张或批量上传；「板端文件」支持输入板端图片
   或文件夹的绝对路径，如 `/userdata/test.jpg`、`/userdata/pictures`。文件夹可勾选
   「包括子文件夹」，点击「读取图片清单」预览后再「开始推理」，也可以直接开始。
4. 右侧紧凑的竖向图片列表支持独立滚动，点击后在左侧预览原图和推理图。
   配置栏支持拖动右侧边缘调整宽度，向左拖窄可收起，收起后可从边缘向右拖开；
   图标按钮也可收起、展开，已选图片和参数会保留。窄屏下预览自动上下排列。
   多模型任务可切换合并结果或单个模型。
   原图和推理图保持比例铺满窗口，超出窗口的边缘会裁切。
5. 单个 YOLO 目标检测模型（`yolov5`、`yolov8_det`）的成功结果可勾选或全选，点击「下载标签 TXT」。
   ZIP 中每张图片对应一个同名 TXT，每行是 `class_id x_center y_center width height`，坐标归一化到 0～1，
   不附带置信度。未检测到目标时生成空 TXT；重名图片按列表序号分目录保存。
   多模型、分割、姿态、旋转框及深度任务不提供此下载。下载包只含标签，不包含图片或 JSON。

板端文件夹按相对路径排序，递归时保留子文件夹名称，不跟随目录符号链接。
仅筛选 JPG/JPEG、PNG、BMP、WebP 文件；空文件夹、不可读路径或存储空间不足会明确报错。
图片数量和整批大小没有固定上限，板端文件夹扫描不会按固定数量截断。
开始时将板端图片复制到任务私有目录；原始图片不会被修改，缓存清理也不会删除原文件。
页面只显示当前批次；刷新后恢复当前批次状态和结果。

也可以在画布模型节点的配置栏点击「图片推理 · 带入本通道启用的模型」。
新标签页会带入当前通道全部启用模型以及推理 ROI，包括画布尚未保存的参数。
画布来源仅用于一次性把相对模型路径转换为绝对路径；图片任务仍由独立引擎运行，不调用业务程序。
图片页面的后续修改仅影响本次图片任务，不写回程序配置。

任务开始后会保存一份参数快照。结果表格显示这份实际参数，随后修改左侧表单不会改变旧任务。
图片在批次内顺序处理，各模型沿用视频的并行执行和 NPU 调度；模型只加载一次。
一张图片解码失败不会中止其他图片。模型加载失败会使整个任务失败；运行期间某个模型失败时，
其余模型的结果保留，并在结果中分别标明成功/失败。

每次可使用 1～8 个模型，图片数量和文件字节数按任务存储目录的实际剩余空间决定。
页面显示扣除预留空间后的可用容量；开始上传时用完整文件清单检查预算，必须保留超过 100 MB
（100 × 1024 × 1024 字节）。图片逐张直接写入任务目录，不经过 multipart 临时副本。
每块数据写入前后都会复查空间；不足时停止上传并删除本批未提交的文件，可重新选择后重试。
板端图片复制、推理输出也检查这个预留空间，不再设置固定的 2 GiB 输出上限和整批运行时限。
解码后的单张图片仍须不超过 16,777,216 像素，以控制图片解码占用的内存。
大量图片只展示前 60 张缩略图，推理和结果列表仍包含所有选中的图片。
取消会请求原生进程停止，正常情况下在当前图片结束后退出，超过 10 秒仍未退出时强制结束。
切换页面不取消任务；控制台重启会把未结束任务标为中断，已完成图片仍可查看。

全设备共用 `/run/lock/rk3588-visual-npu.lock`。视频程序或其他图片任务占用 NPU 时，
新任务会被拒绝；请在程序管理中主动停止视频程序后调试。该页面不会停止运行中的程序。
图片引擎由控制台独立部署，不使用任何程序目录下的 `vision_analysis`。
控制台引擎未安装或版本不兼容时会明确报错，不会回退调用业务程序。

任务默认保存在 `/opt/ai_apps/_console/image_inference`，可用 `IMAGE_INFERENCE_ROOT` 修改。
模型和标签来自 `/opt/ai_apps/<程序包>/assets/`。
独立引擎在 `/opt/ai_apps/_console/inference_runtime`（`IMAGE_INFERENCE_RUNTIME`）。
结果缓存独立于视频程序。后台最多保留最近 5 个已结束批次的缓存，超出的缓存自动清理，
避免结果持续占用磁盘；页面不提供历史列表或复用旧任务参数的功能。
推理结束后，点击“结果”右侧的「清除记录」按钮会先弹出确认提示；确认后才清除全部已结束记录及缓存，包括图片副本、推理图和检测结果。
程序包内的模型、标签、板端原图及正在运行的批次会保留。需要长期保存的检测标签请先下载。
任务和结果接口沿用控制台登录认证。

所有任务 API 均位于 `/api/image-inference`，不带程序名称。模型参数中的文件路径为板端绝对路径。
`GET /api/apps` 提供程序包及其模型、标签清单；前端用程序包目录与资源相对路径组成绝对路径。
图片推理原有的公共模型库查询、上传接口已移除。
`GET /models/info?path=...&model_type=...` 检查所选模型，返回与画布相同的兼容性结果和输入尺寸。
`POST /canvas-preset` 是可选的画布参数转换适配器，不参与独立任务的运行。
电脑上传接口依次调用：

- `POST /jobs`：JSON 请求体 `{"settings":{...},"files":[{"name":"a.jpg","size":1234}]}`，校验容量后返回 `id`。
- `PUT /jobs/{id}/inputs/{index}`：请求体直接发送图片字节，序号从 0 开始，大小必须与清单一致。
- `POST /jobs/{id}/submit`：全部上传完成后开始推理。
- `DELETE /jobs/{id}/upload`：放弃并清理尚未提交的上传；已提交任务仍使用取消推理接口。

`GET /capabilities` 同时返回 `free_bytes`、`min_free_bytes`、`upload_available_bytes`，不再返回固定图片数量或大小上限。
`GET /jobs/current` 返回运行中的批次，空闲时返回最近一批，无批次时返回 `null`。
历史列表 `GET /jobs`、单个任务查询 `GET /jobs/{id}` 和手动删除 `DELETE /jobs/{id}` 已移除。
`DELETE /jobs/completed` 清除已结束批次，返回删除数量 `cleared` 和剩余当前批次 `current`（可为 `null`）。
取消推理、单张结果数据和预览图片接口继续使用批次 ID；JSON 仅用于内部结果解析，不作为文件下载。
`POST /jobs/{id}/labels` 接收 `{"indices":[0,2]}`（图片序号从 0 开始），返回仅含所选 TXT 的 ZIP。
任务必须已结束，所选图片必须推理成功，且任务实际使用单个受支持的目标检测模型；后端会再次校验。
标签按结果的 `coordinate_width`、`coordinate_height` 归一化，ROI 坐标已由引擎还原到整图。
旧的 `GET /jobs/{id}/download` 整批图片打包接口和预览接口的附件下载参数已移除。

模型探测和兼容性规则统一由公共 [model_inspection.h](../../engine/include/model_inspection.h) SDK 实现。
画布和图片页面均通过控制台独立的 `image_inference_runner --inspect-model` 调用 SDK，
Python 的 [model_info.py](../../web_console/backend/services/model_info.py) 只处理路径、缓存和 SDK 结果转发。
开始任务时后端仍会检查；缓存绑定检查引擎与模型文件的版本，文件替换后自动重新探测。
不再依赖画布所属程序的 `vision_analysis` 文件来执行检查。C++ 调用示例、职责边界、缓存和画布流程见
[模型检查 SDK](model-inspection.md)。

板端路径 API（JSON 请求体）：

- `POST /api/image-inference/device-images`：`{"path":"/userdata/pictures","recursive":false}`，返回图片清单和大小。
- `POST /api/image-inference/jobs/from-device`：同上并添加 `settings`，其格式与上传任务一致。
  提交时重新扫描并生成输入快照；后续任务状态、取消、结果和下载接口与电脑上传共用。

## C++ 调用

公共接口：[inference.h](../../engine/include/inference.h)。最小完整示例位于
[engine/sdk_standalone_tests/image_inference/main.cpp](../../engine/sdk_standalone_tests/image_inference/main.cpp)。
该示例只包含公共 SDK 和 OpenCV 头文件，演示同一批图片运行两个不同阈值的模型。

```cpp
#include <inference.h>
#include <opencv2/imgcodecs.hpp>

ChannelModelConfig person;
person.id = "person";
person.model_type = "yolov8_det";
person.model_path = "/userdata/models/person.rknn";
person.label_path = "/userdata/models/labels.txt";
person.obj_thresh = 0.9f;
person.nms_thresh = 0.45f;

ChannelModelConfig pose;
pose.id = "pose";
pose.model_type = "yolov8_pose";
pose.model_path = "/userdata/models/pose.rknn";
pose.obj_thresh = 0.6f;

vision::ImageInferenceOptions options;
options.models = {person, pose};
vision::ImageInferenceSession session(options); // 加载一次，在批次内重复使用。

cv::Mat image = cv::imread("test.jpg");
vision::FrameInferenceResult frame = session.infer(image);
for (const auto &model : frame.models) {
    // model.succeeded 区分「成功但无目标」与「推理失败」。
    // model.error / model.timing / model.input_width / model.input_height 可用于诊断。
}
for (const AlgoResult &target : frame.results) {
    // target.model_id 是 person 或 pose；坐标语义与 ChannelContext::results 相同。
}
vision::draw_inference_results(image, frame);         // 所有模型
// vision::draw_inference_results(image, frame, "pose"); // 只绘制指定模型
cv::imwrite("annotated.jpg", image);
```

构建示例：

```sh
cmake -S engine -B build/engine -DBUILD_IMAGE_INFERENCE_EXAMPLE=ON
cmake --build build/engine --target image_inference_example -j4
build/engine/image_inference_example yolov8_det /absolute/model.rknn /absolute/labels.txt /absolute/image.jpg
```

示例把 `result-0.jpg` 等文件写入当前工作目录。实际项目接入 CMake 时使用
`target_link_libraries(your_tool PRIVATE Vision::Inference)`，对应实际静态库 `vision_inference`。
在包含本仓库 engine CMake 的工程中使用该目标即可传递公共头文件、OpenCV、线程、RGA、
锁定版本 RKNN Runtime 等依赖；无需链接视频主程序，也无需初始化 GTK、GStreamer、业务逻辑或 GPIO。
实现库使用 C++14；公共头文件保持现有 SDK 的隔离边界。

生命周期和结果约定：

- 构造时检查配置、取得 NPU 所有权、加载模型。错误通过异常返回，不构造半可用的会话。
- `infer()` 同步返回，同一个会话的调用串行化。传入非连续 Mat 也受支持；输入图像不会被修改。
  调用期间不要从其他线程修改输入像素。
- 同一 `FrameInferenceResult` 的所有模型来自同一张输入图。`complete()` 表示所有模型成功，
  `succeeded()` 表示至少一个模型成功；成功但零目标也是成功。
- 返回结果可以跨下一次 `infer()` 和会话销毁继续保存。复制结果时，OpenCV Mat 按引用计数共享；
  如果要修改复制品的 mask/depth 像素，先调用 `clone()`。
- 检测框、OBB、关键点、掩码、深度统一映射到 640×640 业务坐标；原图尺寸保存在
  `source_width/source_height`。绘图接口把业务坐标映射回目标图像尺寸。
- 各模型阈值决定哪些检测进入 `results`；标注图只画这些结果。显示分数保留两位小数，
  JSON 和结构体保留原始浮点精度。深度图不是目标置信度，不按检测阈值删除。
- 图片之间不跟踪，`track_id` 为 `-1`。不会执行通道业务回调、区域入侵状态机或报警上报。
- 图像接口供独立调试程序使用；已运行的视频进程内部应直接使用 `ChannelContext` 已发布的结果，
  不要在业务回调中再创建争用 NPU 所有权的图片会话。

## 独立引擎构建与部署

```sh
cmake -S engine -B build/image-inference -DVISION_INFERENCE_ONLY=ON
cmake --build build/image-inference --target image_inference_runner -j4
bash web_console/build_image_runtime.sh /absolute/runtime-directory
```

独立构建不读取 `projects/`，不链接视频业务模块。`image_inference_runner` 和视频程序
链接同一个 `Vision::Inference`，共享模型注册、预处理、后处理、阈值筛选与绘图。
`web_console/build_image_runtime.sh` 一并部署锁定版本的 RKNN Runtime；
`install.sh online/offline` 和完整离线包构建均调用此步骤，新增模型后更新控制台即可使用。

## 命令行批量接口

```sh
/opt/ai_apps/_console/inference_runtime/image_inference_runner --image-inference-capabilities
/opt/ai_apps/_console/inference_runtime/image_inference_runner --infer-images /absolute/job.json
```

`--image-inference-capabilities` 无需初始化 NPU，返回一行
`IMAGE_INFERENCE_CAPABILITIES={...}`，模型类型和结果类型来自同一个 C++ 模型注册表。

任务 JSON 示例（路径建议使用绝对路径）：

```json
{
  "models": [
    {"id": "person", "model_type": "yolov8_det", "model_path": "/data/person.rknn",
     "label_path": "/data/labels.txt", "obj_thresh": 0.9, "nms_thresh": 0.45, "npu_core": -1},
    {"id": "pose", "model_type": "yolov8_pose", "model_path": "/data/pose.rknn",
     "obj_thresh": 0.6, "nms_thresh": 0.45, "npu_core": -1}
  ],
  "images": [{"path": "/data/first.jpg", "name": "first.jpg"},
             {"path": "/data/second.jpg", "name": "second.jpg"}],
  "output_dir": "/data/new-image-task"
}
```

可选 `inference_roi` 使用视频通道同样的 `mode/resize_mode/polygon` 配置。
`output_dir` 的父目录需要已存在；不要重复使用已有任务的输出目录。

```text
output_dir/
├── progress.json             原子更新的批次进度、每张图片状态和错误
├── 000000/
│   ├── result.json           schema_version=1，全部模型状态、耗时、AlgoResult 字段
│   ├── original.jpg          解码后的原图，重新编码为 JPEG
│   ├── annotated.jpg         合并结果
│   ├── model_0.jpg           单模型结果
│   ├── model_0_mask.png      分割模型有掩码时输出，0=背景，其他值=class_id+1
│   └── model_1_depth.npy     深度模型输出，float32、640×640；类型见 depth_kind
└── 000001/ ...
```

掩码和深度文件的存在及名称以 `result.json.models[]` 中的 `mask/depth` 为准。
米制深度与相对逆深度通过 `depth_kind` 区分，后者不能直接作为米制距离。
进程退出码：0 全部成功，1 部分图片或模型失败，2 参数/加载/初始化失败，130 取消。

## 新模型接入位置

1. 在 `engine/src/vision/models/` 实现 `ModelBase`，沿用统一 NPU 执行层，报告实际输入尺寸，
   输出 `AlgoResult`，正确实现阈值、NMS 和输入色彩处理。
2. 在 [model_registry.cpp](../../engine/src/vision/models/model_registry.cpp) 添加一条类型、结果种类、
   标签要求和构造函数注册；实现源文件加入 `engine/cmake/Inference.cmake`。
3. 如果使用已有 detection/OBB/pose/segmentation/depth 字段，图片页面会从能力接口自动列出模型，
   直接复用现有绘图和 JSON 导出。添加新的结果数据类型时，再扩展 `AlgoResult`、公共绘图和序列化。
4. 构建并更新独立图片引擎；视频程序需要使用新模型时再更新它自己的引擎。三者使用同一个注册表源码。
   原有画布模型类型的前端说明/模型信息探测白名单仍需按新类型维护；不要在 Python 或 Web 中重写推理。

关键调用关系：

```text
视频解码 → inference_executor.cpp ─┐
                                  ├→ FrameExecutor → 模型工厂 / ModelBase
图片 Mat → ImageInferenceSession ──┘        │
                                   同一套预处理、ROI、坐标和结果筛选
                                           │
                                  同一套 result_renderer
```

相关验证：`BUILD_IMAGE_INFERENCE_TESTS` 提供多模型尺寸、阈值、异常后复用、空结果与渲染测试；
后台 `tests/test_image_inference.py` 覆盖任务与文件边界，前端同名测试覆盖批量表单、分模型视图和画布预设。
后台测试需要 pytest/httpx，浏览器测试还需要 Python Playwright 和 Chromium。
