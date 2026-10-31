# 模型推理与二次开发统一契约

业务模块消费 `ChannelContext`，不直接加载 RKNN、不操作 NPU/RGA，也不按模型输入分辨率缩放结果。底层统一负责模型实例、预处理、推理、后处理、ROI 还原、来源标识及并发调度。

## 统一入口

```cpp
if (!ctx->infer_enabled || !ctx->inference_valid || !ctx->results) return;

TargetQuery query;
query.model_id = "detector";              // 配置 models[].id；留空表示不限模型
query.labels = {"person"};
query.min_score = 0.5f;
query.roi = ROI_FRAME;                    // 或 roi_find(ctx, "区域名")
auto found = ctx->query_targets(query);
if (found.valid() && found.best) {
    const AlgoResult &target = *found.best;
    // target.box / target.score / target.track_id
}

for (const AlgoResult &result : *ctx->results) {
    if (result.model_id != "obb") continue;
    if (result.has_obb) {
        const auto &corners = result.obb_corners; // 四角，已还原到业务坐标
        (void)corners;
    }
}
if (const auto *seg = ctx->segmentation_result("segmenter")) {
    const cv::Mat &classes = seg->boxMask; // 整帧 CV_8UC1：0 背景，class_id+1 前景
    (void)classes;
}
if (const auto *depth = ctx->depth_result("depth")) {
    const float value = depth->depth_at(320, 320);
    // depth->is_metric_depth(): 米；is_relative_inverse_depth(): 相对逆深度，无单位
    (void)value;
}
```

`query.model_type` 可进一步限定类型；标签、模型 ID、类型、置信度与业务 ROI 条件同时生效。查询返回的指针只在当前回调有效，不要跨帧保存。若需要长期持有图片/掩码并修改内容，应 `clone()`。深度和分割访问器在本帧推理无效时返回空指针。

每条结果统一包含 `model_id/model_type/model_index`、`chn_id/frame_id/timestamp_ms`；模型不同不改变读取方式，但输出载荷具有各自语义：

| 模型类型 | 结果载荷 |
|---|---|
| yolov5、yolov8_det | box、label、class_id、score |
| yolov8_obb | 上述字段 + has_obb、obb_corners；box 是包围框 |
| yolov8_pose、yolo26_pose | 上述字段 + keypoints、keypoint_scores、pose_schema |
| yolov5_seg | 检测结果 + 整帧类别掩码，使用 segmentation_result 查找载荷 |
| yolo26_depth | depth_map + Metric，单位米，无效值 0 |
| zip_depth | depth_map + RelativeInverse，相对逆深度，无效值 NaN |

深度不参与目标计数、目标 NMS 或目标跟踪。普通检测与 OBB 的传统业务 ROI/跟踪仍使用 `box` 包围框；需要精确旋转几何时读取四角。分割掩码是模型的整帧类别平面，不是每个目标的独立实例掩码。

## 尺寸与 ROI

模型输入宽高从 RKNN 元数据读取；同通道的每个子模型独立准备实际输入尺寸。业务坐标固定为 **640×640**，与视频分辨率、640/768/960/1088 等模型尺寸无关。切换尺寸时业务模块无需修改像素坐标。框、关键点、OBB 四角、分割和深度分别按同一个几何变换还原。

支持整帧（`mode` 留空）、`full_frame_roi_filter`、`roi_only`、`full_plus_roi`，以及 ROI 的 `stretch/letterbox/expand`：

- stretch：ROI 拉伸到模型尺寸；OBB 四角分别做非等比变换。
- letterbox：保留比例并补边；返回结果去掉补边、映射回全图，丢弃补边目标。
- expand：扩大取景框保留上下文；ROI 批次输出仍按用户区域过滤。
- 多边形 stretch/letterbox 会在输入中遮蔽区域外像素；expand 保留上下文。
- 整帧+ROI 使用独立批次，再按模型隔离合并；分割掩码及米制深度可用 ROI 结果覆盖相应区域。
- **ZipDepth 相对深度例外**：不同裁剪的数值尺度无可比性，因此 full_plus_roi 使用整帧结果；roi_only 仍支持裁剪。不能把它当米制深度或与独立 ROI 图强行拼接。

CPU 回退和 RGA 路径使用同一取景几何，生产调用统一持有实例锁。不同输入尺寸不代表任意导出格式都可用：各解码器仍要求对应的检测头/关键点结构，错误格式会在加载或 Web 校验阶段拒绝。分割 proto 的空间尺寸已动态读取，32 个原型通道的格式约束保留。

## 本次审查修复

1. YOLOv8 检测、姿态、分割原排序会移动置信度数组，随后按原候选索引读取，可能错配；现在只排序索引，保留分数与目标对应。
2. 分割 NMS 按排序后的真实类别索引抑制，避免跨类别误抑制。
3. 分割 proto 从模型查询实际高宽，移除固定 160×160；Web 与运行时采用一致约束。
4. 各模型推理入口清空输出容器，避免复用容器时保留旧帧；量化混合输出在传统检测/分割中统一要求运行库转浮点，避免误按 INT8 解释。
5. 目标查询增加模型 ID／类型筛选；深度、分割都有当前帧访问器。
6. 组合及最终筛选保留各模型分割载荷，首个目标被过滤时不会把该模型的掩码丢失或挂到其他模型上。
7. 检测头按实际输入尺寸校验步长和通道，防止不兼容模型触发越界解码。

正式源码和可重复的回归测试保留。测试运行目录、临时日志、下载归档和测试构建副本清理后，验证结论保留在本文。

## 验证结果（2026-10-29）

- 板端模型：yolov5s、yolov8、1088 安全帽、yolov8n_obb、yolov8_pose、yolov5_seg、yolo26n-depth、768 ZipDepth，8 个文件覆盖 7 种模型类型，全部通过。
- 每个文件创建 3 个同核实例，检查复用容器时 24 次 CPU 推理的结果一致性及 24 次 DMA 入口调用，并逐实例实跑 stretch/letterbox/expand 的 ROI 输入与业务坐标还原。
- CPU 回归 23 项全部通过；最终改动后的 7 项针对性回归全部通过。后端模型检查 32 项全部通过。
- 合成张量检查覆盖 640/768/960/1088 和矩形检测头、动态分割 proto；OBB 四角覆盖正方形、矩形输入和三种 ROI resize 模式。现有真实分割模型为 640，其他 proto 尺寸为结构/几何回归，不冒充真实模型精度测试。
- YOLO26 Pose 已检查接口与动态尺寸代码；当前 assets 无对应 RKNN，未做该类型的新一轮板端推理。其余现有 7 类实际模型均已实跑。
- 新构建已同步到 `/opt/ai_apps/test_obb/vision_analysis`；控制台模型校验已更新并重启。其他历史发布包维持其版本，若用于正式业务应重新打包更新。

清理了本次及 OBB 测试的 86 个生成路径（测试构建副本、可重建测试二进制/对象、日志、结果图片和下载归档），合计约 228.4 MiB。保留生产可执行文件、模型与标签、示例源码、回归测试源码及部署回滚备份。
