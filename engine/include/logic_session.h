/** @file logic_session.h
 * @brief 用图片、人工检测结果或录像逐帧执行正式业务回调，无需启动视频主程序。
 */
#pragma once

#include <channel.h>
#include <events.h>
#include <memory>
#include <parameters.h>

namespace vision
{
/** 一帧借用输入；results、rois 均使用固定的 640×640 业务坐标。
 * source 为任意分辨率的 BGR 原图；business 可选，必须为 640×640 BGR。
 * 未提供 business 时，只在回调请求 business_frame() 时缩放 source。
 * 所有指针及像素/容器必须保持有效，直到下一次 run() 或 session 销毁。
 * 不提供图像也可测试目标查询、状态、参数、动作与事件逻辑。 */
struct LogicFrame
{
    const cv::Mat *source = nullptr;
    const cv::Mat *business = nullptr;
    std::vector<AlgoResult> *results = nullptr;
    const std::vector<RoiZone> *rois = nullptr; // nullptr 使用 options.channel.roi_zones
    int64_t frame_id = -1;                      // -1 由 session 递增
    uint64_t timestamp_ms = 0;                  // 由调用方控制，可直接快进模拟时间
    uint64_t unix_ms = 0;
    bool inference_valid = true; // 接真实推理时传 FrameInferenceResult::succeeded()；空有效结果仍为 true
    bool inference_enabled = true; // 与 channel.infer_enable 同时满足才开启
};

struct LogicSessionOptions
{
    ChannelConfig channel;
    ChannelLogicFunc logic = nullptr; // 可直接传单个 cpp 内的函数；为空时按 channel.logic 查注册表
    ChannelLogicActionFunc action = nullptr;
    EventSink *events = nullptr; // 可选，借用；空时收集到 events()，不生成文件或网络请求
};

/** 捕获提交时的请求与绘图，便于断言；不模拟持久化、合并或远端投递。 */
struct LogicEvent
{
    int64_t frame_id = 0;
    uint64_t timestamp_ms = 0;
    EventRequest request;
    std::vector<DrawCommand> drawing;
};

/** 每路创建一个 session；状态相互隔离，调用方负责串行调用。
 * 内部直接绑定并调用同一个 ChannelContext/ChannelLogicFunc，不创建工作线程。
 * 构造/参数变更失败抛 std::invalid_argument；错误不覆盖已生效参数。
 * 使用命名模块参数时，先 set_logic_catalog() 注册与正式程序相同的 schema。
 * 参数为空的单 cpp 回调可直接使用默认 channel.logic=""、parameters="{}"。 */
class LogicSession
{
  public:
    /** @brief 创建独立业务状态，直接调用 options.logic 或按配置名称查找回调。
     * @note 参数 Schema 先注册；缺回调或参数无效抛 invalid_argument；借用的 events 接收器须更晚销毁。
     * 示例：
     * @code
     * vision::LogicSessionOptions options;
     * options.logic = [](ChannelContext *c) {
     *     c->publish_int("count", 0);
     * };
     * vision::LogicSession session(options);
     * @endcode
     */
    explicit LogicSession(const LogicSessionOptions &options);
    /** @brief 释放本会话状态、输出及事件；须等待当前 run()/dispatch() 结束。
     * 示例：
     * @code
     * {
     *     vision::LogicSessionOptions options;
     *     options.logic = [](ChannelContext *) {
     *     };
     *     vision::LogicSession session(options);
     * }
     * @endcode
     */
    ~LogicSession();

    /** @brief 执行一帧真实业务回调；清空上一帧输出/绘图/事件，保留业务 state。
     * @note 输入图像/容器需保持到下一次 run() 或会话销毁；同一 session 由调用方串行访问。
     * 示例：
     * @code
     * std::vector<AlgoResult> targets;
     * vision::LogicFrame frame;
     * frame.results = &targets;
     * frame.timestamp_ms = 1000;
     * session.run(frame);
     * @endcode
     */
    void run(const LogicFrame &frame);
    /** @brief 在当前帧上下文执行动作回调；无匹配处理器时 handled=false。
     * @note 必须先 run()，否则抛 logic_error；action.logic_name 非空时须匹配当前模块。
     * 示例：
     * @code
     * LogicAction action;
     * action.name = "reset";
     * auto result = session.dispatch(action);
     * @endcode
     */
    LogicActionResult dispatch(const LogicAction &action);
    /** @brief 校验并更新参数，按 Schema 决定保留或重置状态。
     * @return 返回热重载级别；参数无效或要求重建会话时抛 invalid_argument，旧参数保持不变。
     * 示例：
     * @code
     * auto impact = session.set_parameters("{}");
     * @endcode
     */
    LogicReloadImpact set_parameters(const std::string &json);
    /** @brief 仅释放跨帧 state，下次 get_state<T>() 重新构造；不清空本帧输出/事件。
     * 示例：
     * @code
     * session.reset_state();
     * @endcode
     */
    void reset_state();

    /** @brief 借用最近帧的上下文；其内容在下一次 run() 被更新，勿异步保存内部指针。
     * 示例：
     * @code
     * const auto selected = session.context().query_targets();
     * @endcode
     */
    const ChannelContext &context() const;
    /** @brief 借用当前帧发布的输出；下一次 run() 会清空，跨帧需要自行复制。
     * 示例：
     * @code
     * int64_t count = 0;
     * bool ok = session.outputs().try_get_int("count", &count);
     * @endcode
     */
    const LogicOutputSet &outputs() const;
    /** @brief 借用当前帧绘图指令；下一次 run() 会清空。
     * 示例：
     * @code
     * std::vector<DrawCommand> saved = session.drawing();
     * @endcode
     */
    const std::vector<DrawCommand> &drawing() const;
    /** @brief 借用本帧默认收集器捕获的事件；自定义 sink 模式下不会自动收集。
     * 示例：
     * @code
     * for (const auto &event : session.events())
     * {
     *     std::string type = event.request.event_type;
     * }
     * @endcode
     */
    const std::vector<LogicEvent> &events() const;
    /** @brief 借用业务启用的显示底图；未调用显示接口则 nullptr，底图仍可能为空。
     * 示例：
     * @code
     * if (const auto *frame = session.display_frame())
     * {
     *     cv::Mat saved = frame->clone();
     * }
     * @endcode
     */
    const cv::Mat *display_frame() const;

    /** @brief 禁止复制构造，避免复制资源或跟踪状态；共享实例请借用引用。
     * 示例：
     * @code
     * auto &borrowed = session;
     * (void)borrowed;
     * @endcode
     */
    LogicSession(const LogicSession &) = delete;
    /** @brief 禁止复制赋值，避免复制资源或跟踪状态；共享实例请借用引用。
     * 示例：
     * @code
     * auto &borrowed = session;
     * (void)borrowed;
     * @endcode
     */
    LogicSession &operator=(const LogicSession &) = delete;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace vision
