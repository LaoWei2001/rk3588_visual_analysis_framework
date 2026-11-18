/** @file channel.h
 * @brief 编写通道处理逻辑：读图像和检测结果、读设置、保存计数、向全局逻辑提供数据。
 * 程序调用你写的函数时会传入 ChannelContext *ctx，直接使用它，不需要自己创建。
 * 下方示例通常写在这个函数里；REGISTER_* 按说明写在函数体外。
 * 检测框、绘图和网页关注区域统一按 640×640 计算位置，摄像头原图可能是其他尺寸。
 */
#pragma once

#include <actions.h>
#include <config_types.h>
#include <drawing.h>
#include <memory>
#include <outputs.h>
#include <results.h>
#include <snapshot.h>
#include <types.h>
#include <utility>

/*======================== 当前通道的信息和操作 ========================*/
// 自己的计数和计时数据放到 get_state<T>() 中；需要在网页调整的设置写进 logic.json。

/* 前置声明逻辑函数类型 */
struct ChannelContext;
class LogicParameterSet;
class EventSink;
/** @brief 通道处理函数的写法：程序把当前画面信息交给你，你在函数里完成判断或绘图。
 * @param ctx 程序提供的当前通道信息，直接用它读图像、结果和参数。
 * @return 没有返回值；需要传给全局逻辑的数据，用 ctx->publish_*() 写出。
 * 函数结束后，不要保存 ctx 留到其他线程或下次处理时使用。
 * 示例：
 * @code
 * void count_people(ChannelContext *ctx)
 * {
 *     draw_text(ctx, "运行中", {20, 40});
 * }
 * REGISTER_LOGIC(count_people);
 * @endcode
 */
typedef void (*ChannelLogicFunc)(struct ChannelContext *ctx);
/** @brief 处理发给当前逻辑的按钮或控制命令，例如显示提示、清零。
 * @param ctx 当前通道的信息，可用它绘图，或用 get_state<T>() 修改自己的计数。
 * @param action 本次命令；先判断非空，再读 name（命令名）和 payload_json（附加参数）。
 * @return handled=true 表示已处理，false 表示未处理；message 填给用户看的说明。
 * 使用 REGISTER_LOGIC_ACTION 把这个函数与对应通道逻辑关联起来。
 * 示例：
 * @code
 * LogicActionResult handle_command(ChannelContext *ctx, const LogicAction *action)
 * {
 *     if (!action || action->name != "show_notice")
 *         return {false, "不支持这个命令"};
 *     draw_text(ctx, "已收到命令", {20, 40});
 *     return {true, "已显示提示"};
 * }
 * @endcode
 */
typedef LogicActionResult (*ChannelLogicActionFunc)(struct ChannelContext *ctx, const LogicAction *action);
/** @brief 供程序取图时使用的函数类型，普通通道逻辑直接调用 business_frame() 或 source_frame()。
 * @param opaque 由程序传给取图函数的数据；普通通道逻辑不需要填写。
 * @return 取图函数返回图片地址；取不到图时返回 nullptr。
 * 示例：
 * @code
 * const cv::Mat *image = ctx->business_frame();
 * if (image)
 * {
 *     cv::Mat saved = image->clone();
 * }
 * @endcode
 */
typedef const cv::Mat *(*ChannelFrameGetter)(void *opaque);

struct ChannelContext
{
    /** @brief 取得检测框和绘图所用画面的宽度，用来计算横向位置。
     * @return 固定返回 640，单位是像素；不是摄像头原始宽度。
     * 不需要传入参数。
     * 示例：
     * @code
     * int width = ctx->business_width(); // 640
     * @endcode
     */
    int business_width() const
    {
        return business_coordinates::WIDTH;
    }
    /** @brief 取得检测框和绘图所用画面的高度，用来计算纵向位置。
     * @return 固定返回 640，单位是像素；不是摄像头原始高度。
     * 不需要传入参数。
     * 示例：
     * @code
     * int height = ctx->business_height(); // 640
     * @endcode
     */
    int business_height() const
    {
        return business_coordinates::HEIGHT;
    }
    /** @brief 同时取得检测框和绘图所用画面的宽高，适合创建同样大小的图片。
     * @return 返回 cv::Size(640, 640)，两个数分别是宽、高，单位像素。
     * 不需要传入参数。
     * 示例：
     * @code
     * cv::Size size = ctx->business_size();
     * cv::Mat image(size, CV_8UC3, cv::Scalar(0, 0, 0));
     * @endcode
     */
    cv::Size business_size() const
    {
        return business_coordinates::size();
    }
    /** @brief 取得检测和绘图画面的中心点，方便在正中间画圆或放标记。
     * @return 返回 cv::Point(320, 320)，两个数分别是横、纵坐标。
     * 不需要传入参数。
     * 示例：
     * @code
     * cv::Point center = ctx->business_center();
     * draw_circle(ctx, center, 30);
     * @endcode
     */
    cv::Point business_center() const
    {
        return business_coordinates::center();
    }

    /* ---- 唯一通道身份：config.channels[].id ---- */
    int chnId = -1;

    /* ---- 当前帧数据 ---- */
    /* 原始视频图片的宽高，例如 1920×1080；尚未收到图片时可能为 0。
     * business_frame() 和检测框、绘图仍按 640×640 使用。 */
    int src_width = 0;
    int src_height = 0;

    /** @brief 取得当前的 640×640 彩色图片，图片上的位置与检测框、网页所画区域一致。
     * @return 返回图片地址；没有图时返回 nullptr。图片按蓝、绿、红三个颜色通道存放。
     * 不需要传入参数。不要修改或释放返回的原图；需要保留时用 clone() 复制。
     * 示例：
     * @code
     * if (const cv::Mat *image = ctx->business_frame())
     * {
     *     cv::Mat saved = image->clone(); // 复制后可以自己修改或留到以后使用
     * }
     * @endcode
     */
    const cv::Mat *business_frame() const;
    /** @brief 取得摄像头或视频原始大小的彩色图片，适合查看清晰细节。
     * @return 返回原始尺寸的图片地址；没有图时返回 nullptr，颜色顺序是蓝、绿、红。
     * 不需要传入参数。需要修改或保留图片时用 clone() 复制。
     * 检测框按 640×640 给出；画到原图上前，需要按原图宽高换算位置。
     * 示例：
     * @code
     * if (const cv::Mat *image = ctx->source_frame())
     * {
     *     cv::Mat saved = image->clone();
     * }
     * @endcode
     */
    const cv::Mat *source_frame() const;

  private:
    friend struct VisionContextAccess;
    /* 取图所需的数据由程序填写；在通道函数里用 business_frame()/source_frame()。 */
    ChannelFrameGetter business_frame_getter = nullptr;
    ChannelFrameGetter source_frame_getter = nullptr;
    void *frame_getter_opaque = nullptr;

  public:
    // 当前处理的画面编号
    int64_t frame_id = 0;
    /* 当前帧的分析时间（毫秒），用来计算目标持续多久。文件暂停不计时，摄像头按实时推进。
     * 起点由框架选择，只用于同一通道的时间差；跳转时框架重置业务状态。 */
    uint64_t timestamp_ms = 0;
    /* 帧进入板端分析管线时的现实时间（单调毫秒），供通信超时等实际耗时判断。
     * 普通视频业务计时仍用 timestamp_ms；显示日期用 unix_ms。 */
    uint64_t real_time_ms = 0;
    uint64_t unix_ms = 0; /* 当前画面的日期时间，按毫秒记录，time_str() 会把它转成文字。 */
    // 本次与上次处理画面的分析时间差（毫秒）；文件暂停期间不累计。
    float dt_ms = 0.0f;
    /** 当前帧在视频文件中的位置，单位毫秒，0 表示文件开头。
     * 文件播放暂停时不推进；跳转或循环播放时可能倒退，不能跨跳转直接计算持续时间。
     * RTSP、USB 或文件没有有效时间时为 -1；普通持续时间判断直接用 timestamp_ms。
     * 示例：
     * @code
     * if (ctx->video_time_ms >= 0)
     * {
     *     double seconds = ctx->video_time_ms / 1000.0;
     *     // seconds 是当前画面在文件中的秒数。
     * }
     * @endcode
     */
    int64_t video_time_ms = -1;
    /* 当前画面的检测结果，已经按模型设置的分数、类别等条件筛选过。
     * 可以继续筛选自己关心的目标，但这里无法找回先前被过滤掉的目标。 */
    std::vector<AlgoResult> *results = nullptr;

    /** @brief 取得模型标出的像素区域，例如人、道路分别覆盖了图片的哪些位置。
     * @param model_id 配置里的模型 ID，例如 "seg"；不填或填空字符串时取第一个有此类结果的模型。
     * @return 找到时返回结果地址，区域图在 boxMask 中；未找到或当前推理不可用时返回 nullptr。
     * 这里只取得结果，不会重新运行模型。返回的地址只在当前处理函数中使用。
     * 示例：
     * @code
     * const AlgoResult *result = ctx->segmentation_result("seg");
     * if (result)
     * {
     *     cv::Mat mask = result->boxMask.clone(); // 复制模型给出的区域图
     * }
     * @endcode
     */
    const AlgoResult *segmentation_result(const std::string &model_id = "") const
    {
        if (!inference_valid || !infer_enabled)
            return nullptr;
        if (results)
            for (const auto &result : *results)
                if (!result.boxMask.empty() && (model_id.empty() || result.model_id == model_id))
                    return &result;
        return nullptr;
    }

    /** @brief 取得深度模型的结果，用来判断画面中某个位置的远近。
     * @param model_id 配置里的模型 ID；不填或填空字符串时取第一个深度结果。
     * @return 找到时返回结果地址；未找到、推理关闭或结果不可用时返回 nullptr。
     * 先检查 depth_valid_at(x,y)；只有 is_metric_depth() 为 true 时数值才表示米。
     * 其他深度模型通常只表示相对远近，不能直接当米用；结果地址只在当前处理函数中使用。
     * 示例：
     * @code
     * const AlgoResult *result = ctx->depth_result();
     * if (result && result->is_metric_depth() && result->depth_valid_at(320, 320))
     * {
     *     float distance_m = result->depth_at(320, 320); // 画面中心的距离，单位米
     * }
     * @endcode
     */
    const AlgoResult *depth_result(const std::string &model_id = "") const
    {
        if (!inference_valid || !infer_enabled)
            return nullptr;
        if (results)
            for (const auto &result : *results)
                if (result.is_depth() && (model_id.empty() || result.model_id == model_id))
                    return &result;
        return nullptr;
    }

    /* ---- 当前通道设置，只能读取，不要修改 ---- */
    const ChannelConfig *config = nullptr;

    /* ---- 当前逻辑的参数；缺省值和取值范围在 logic.json 里声明 ---- */
  private:
    const LogicParameterSet *logic_parameters = nullptr;

  public:
    /** @brief 检查当前逻辑有没有某个参数，读取可选参数前可以先问一下。
     * @param key logic.json 里声明的参数名，例如 "hold_ms"；不是界面上显示的中文标题。
     * @return 有这个参数时 true，没有或 key 为 nullptr 时 false；使用默认值的参数也算存在。
     * 示例：
     * @code
     * if (ctx->has_param("hold_ms"))
     * {
     *     int64_t hold_ms = ctx->param_int("hold_ms");
     * }
     * @endcode
     */
    bool has_param(const char *key) const;
    /** @brief 读取小数参数，例如最低识别分数。
     * @param key logic.json 里声明的参数名，参数类型应为 number 或 integer。
     * @return 返回参数数值；找不到、类型不对或数值太大时返回 0。
     * 界面没有单独设置时，读取 logic.json 中的默认值；本函数只读取，不修改参数。
     * 示例：
     * @code
     * float min_score = ctx->param_float("min_score");
     * @endcode
     */
    float param_float(const char *key) const;
    /** @brief 读取整数参数，例如人数上限或等待毫秒数。
     * @param key logic.json 里声明的参数名，参数类型应为 integer。
     * @return 返回整数；找不到或类型不对时返回 0，不会把小数自动转为整数。
     * 界面没有单独设置时，读取 logic.json 中的默认值；本函数只读取，不修改参数。
     * 示例：
     * @code
     * int64_t hold_ms = ctx->param_int("hold_ms");
     * @endcode
     */
    int64_t param_int(const char *key) const;
    /** @brief 读取开关参数，例如是否启用报警。
     * @param key logic.json 里声明的参数名，参数类型应为 boolean。
     * @return 返回 true 或 false；找不到或类型不对时返回 false。
     * 界面没有单独设置时，读取 logic.json 中的默认值；本函数只读取，不修改参数。
     * 示例：
     * @code
     * bool alarm_enabled = ctx->param_bool("alarm_enabled");
     * @endcode
     */
    bool param_bool(const char *key) const;
    /** @brief 读取文字参数，例如要检测的类别名。
     * @param key logic.json 里声明的参数名，参数类型应为 string。
     * @return 返回文字内容；找不到或类型不对时返回空字符串 ""。
     * 界面没有单独设置时，读取 logic.json 中的默认值；本函数只读取，不修改参数。
     * 示例：
     * @code
     * std::string label = ctx->param_string("target_label");
     * @endcode
     */
    std::string param_string(const char *key) const;
    /** @brief 读取数组或对象参数，供你继续解析里面的多项设置。
     * @param key logic.json 里声明的参数名，参数类型应为 array 或 object。
     * @return 返回 JSON 文字，例如 [1,2]；找不到或类型不对时返回空字符串 ""。
     * 界面没有单独设置时，读取 logic.json 中的默认值；本函数只读取，不修改参数。
     * 示例：
     * @code
     * std::string zones = ctx->param_json("zones");
     * @endcode
     */
    std::string param_json(const char *key) const;

    /* ---- 提供给全局逻辑读取的数据 ----
     * 在 logic.json 的 outputs 中声明名称和类型；需要持续提供的数据每次处理都要填写。 */
  private:
    LogicOutputSet *outputs = nullptr;

  public:
    /** @brief 把当前画面的文字交给全局逻辑，供多个通道一起判断。
     * @param key 输出名称，应与 logic.json 的 outputs 声明一致，类型填 string。
     * @param value 要传出的文字，例如 "person"。
     * @return 没有返回值；同一名称再次写入会替换本次旧值，空名称会被忽略。
     * 每次通道处理都会重新收集这些值，需要持续提供的数据应每次都调用；这不是事件上报。
     * 示例：
     * @code
     * ctx->publish_string("target_label", "person");
     * @endcode
     */
    void publish_string(const char *key, const std::string &value) const;
    /** @brief 把当前画面的小数交给全局逻辑，供多个通道一起判断。
     * @param key 输出名称，应与 logic.json 的 outputs 声明一致，类型填 number。
     * @param value 要传出的数值，例如识别分数 0.9。
     * @return 没有返回值；同一名称再次写入会替换本次旧值，空名称会被忽略。
     * 每次通道处理都会重新收集这些值，需要持续提供的数据应每次都调用；这不是事件上报。
     * 示例：
     * @code
     * ctx->publish_number("best_score", 0.9);
     * @endcode
     */
    void publish_number(const char *key, double value) const;
    /** @brief 把当前画面的整数交给全局逻辑，供多个通道一起判断。
     * @param key 输出名称，应与 logic.json 的 outputs 声明一致，类型填 integer。
     * @param value 要传出的整数，例如检测到的人数。
     * @return 没有返回值；同一名称再次写入会替换本次旧值，空名称会被忽略。
     * 每次通道处理都会重新收集这些值，需要持续提供的数据应每次都调用；这不是事件上报。
     * 示例：
     * @code
     * ctx->publish_int("person_count", 3);
     * @endcode
     */
    void publish_int(const char *key, int64_t value) const;
    /** @brief 把当前画面的开关结果交给全局逻辑，供多个通道一起判断。
     * @param key 输出名称，应与 logic.json 的 outputs 声明一致，类型填 boolean。
     * @param value 要传出的 true/false，例如是否发现危险。
     * @return 没有返回值；同一名称再次写入会替换本次旧值，空名称会被忽略。
     * 每次通道处理都会重新收集这些值，需要持续提供的数据应每次都调用；这不是事件上报。
     * 示例：
     * @code
     * ctx->publish_bool("alarm_active", true);
     * @endcode
     */
    void publish_bool(const char *key, bool value) const;
    /** @brief 把当前画面的一组数据交给全局逻辑，供多个通道一起判断。
     * @param key 输出名称，应与 logic.json 的 outputs 声明一致，类型填 json。
     * @param json 合法的 JSON 文字，例如 [1,2]；本函数不检查文字是否写对。
     * @return 没有返回值；同一名称再次写入会替换本次旧值，空名称会被忽略。
     * 每次通道处理都会重新收集这些值，需要持续提供的数据应每次都调用；这不是事件上报。
     * 示例：
     * @code
     * ctx->publish_json("track_ids", "[1,2]");
     * @endcode
     */
    void publish_json(const char *key, const std::string &json) const;

    /* ---- ROI (已缩放到业务坐标系(640×640)) ----
     * rois 是本通道全部 ROI 区域；单个区域用 roi_polygon_at()/roi_by_name() 获取。 */
    const std::vector<RoiZone> *rois = nullptr;

    /* 绘图和事件所需的数据由程序准备；使用下方公开函数即可。 */
  private:
    std::vector<DrawCommand> *draw_cmds = nullptr;
    EventSink *event_sink = nullptr;

  public:
    /** @brief 把你处理好的图片作为当前显示画面，例如显示裁剪图或灰度图。
     * @param frame 非空图片，尺寸不限；支持 CV_8UC1（灰度）、CV_8UC3（普通彩色）和 CV_8UC4（带透明通道）。
     * @return true 表示已接受这张图；空图、格式不支持或当前不能显示时返回 false。
     * 调用成功后不要再修改传入图片；它不会改变模型本次的识别结果。
     * 示例：
     * @code
     * if (const cv::Mat *image = ctx->source_frame())
     * {
     *     cv::Mat replacement = image->clone();
     *     bool shown = ctx->replace_display_frame(replacement);
     * }
     * @endcode
     */
    bool replace_display_frame(cv::Mat frame);

    /* 两种显示接口共用底图；replace_display_frame 可改变其尺寸。 */
  private:
    cv::Mat *canvas = nullptr;   /* 两种显示接口共用的本帧输出缓冲 */
    bool *show_canvas = nullptr; /* 任一显示接口成功调用后置 true */
  public:
    /** @brief 拿到一张可以直接修改的显示图片，修改后程序就用它显示。
     * @return 返回可修改的图片；没有可用图像时图片可能为空，先用 empty() 判断。
     * 不需要传入参数。通常从当前 640×640 图片开始；若已 replace_display_frame，则继续修改替换图。
     * 只在程序提供的 ctx 上调用；自己创建的 ctx 无法显示时会抛出 std::logic_error。
     * 示例：
     * @code
     * cv::Mat &canvas = ctx->display_canvas(); // 保留 &，直接修改这张图
     * if (!canvas.empty())
     * {
     *     canvas.setTo(cv::Scalar(0, 0, 0)); // 把本次显示画面涂黑
     * }
     * @endcode
     */
    cv::Mat &display_canvas();

    /* ---- 需要从这张画面保留到下张画面的数据 ---- */
    // 用 get_state<T>() 取得自己的计数、计时等数据。
    std::shared_ptr<void> *state = nullptr;

    /** @brief 取得本通道一直保留的数据对象，适合记录计数、计时起点等。
     * @param args 创建 T 对象需要的参数；普通结构体可不传，这些参数只在第一次创建时使用。
     * @return 返回你的 T 对象地址；第一次自动创建，以后继续使用；当前 ctx 无法保存状态时返回 nullptr。
     * T 填你自己定义的结构体名；同一通道逻辑和它的命令处理函数必须始终用同一种 T。
     * 重置逻辑或修改某些参数后对象可能重建；每次处理重新调用本函数获取，不要另存地址长期使用。
     * 示例：
     * @code
     * struct State
     * {
     *     int calls = 0;
     * };
     * State *state = ctx->get_state<State>();
     * if (state)
     * {
     *     ++state->calls; // 下一张画面仍能读到这个计数
     * }
     * @endcode
     */
    template <typename T, typename... Args> T *get_state(Args &&...args) const
    {
        if (!state)
            return nullptr;
        if (!*state)
            *state = std::make_shared<T>(std::forward<Args>(args)...);
        return static_cast<T *>(state->get());
    }

    /* ---- 是否开启推理 ---- */
    int infer_enabled = 0;
    /* true 表示本次有可用的推理结果，可能并没有检测到任何目标。
     * 配置多个模型时，只要其中一部分成功就可能为 true。 */
    bool inference_valid = false;

    /* ---- 每秒处理的画面数：infer_fps 为推理速度，disp_fps 为显示速度 ---- */
    float infer_fps = 0.0f;
    float disp_fps = 0.0f;

    /** @brief 从当前检测结果里挑选你关心的目标，得到数量和分数最高的一个。
     * @param query 筛选条件；不传则查询整张图的全部检测目标。labels 填模型的类别名，min_score 填 0~1。
     * @return 先看 valid()；成功后 count 是数量，best 指向分数最高的目标，没有目标时 best 为 nullptr。
     * query.roi 默认 ROI_FRAME（整图），也可填 roi_find() 的区域编号；labels 为空表示不限类别。
     * query.model_id 填配置里的模型 ID，model_type 填类型名（如 yolov8_det）；留空则不限制模型。
     * query.anchor 默认 TargetAnchor::Center（框中心），也可用 BottomCenter（框底边中点）。
     * valid() 为 false 表示推理不可用或条件/区域有误，不能当成“检测到 0 个”；具体看 status。
     * best 只在当前处理函数中使用；多个模型识别到同一物体时仍会分别计数。
     * 示例：
     * @code
     * TargetQuery query;
     * query.labels = {"person"};
     * query.min_score = 0.7f;
     * const auto selected = ctx->query_targets(query);
     * if (selected.valid())
     * {
     *     ctx->publish_int("person_count", selected.count);
     * }
     * @endcode
     */
    TargetQueryResult query_targets(const TargetQuery &query = TargetQuery()) const;

    /* 网页上为本通道画出的关注区域（ROI）。可以按编号或名称查找，边界点的位置按 640×640 填写。 */

    /** @brief 查看这个通道在网页上配置了几个关注区域（ROI），方便逐个处理。
     * @return 返回区域数量；没有配置时为 0，编号从 0 到数量减 1。
     * 不需要传入参数；这里只数区域，不检查区域是否画得正确。
     * 示例：
     * @code
     * int count = ctx->roi_count();
     * for (int i = 0; i < count; ++i)
     * {
     *     const RoiZone *zone = ctx->roi_at(i);
     *     draw_polyline(ctx, zone->polygon);
     * }
     * @endcode
     */
    int roi_count() const;

    /** @brief 按编号取得一个网页关注区域，查看它的名称和边界点。
     * @param idx 区域编号，从 0 开始；第一个区域填 0，第二个填 1。
     * @return 找到时返回区域地址；编号不存在时返回 nullptr。name 是名称，polygon 是边界点列表。
     * 点的位置按 640×640 图片给出；不要修改区域，也不要把地址留到下次处理。
     * 示例：
     * @code
     * if (const RoiZone *zone = ctx->roi_at(0))
     * {
     *     draw_polyline(ctx, zone->polygon);
     * }
     * @endcode
     */
    const RoiZone *roi_at(int idx) const;

    /** @brief 取得一个网页关注区域的所有边界点，常用于画出区域轮廓。
     * @param idx 从 0 开始的区域编号，可先用 roi_count() 查看数量。
     * @return 返回边界点列表的地址，点按 640×640 图片给出；编号不存在时返回 nullptr。
     * 只在当前处理函数中读取；画封闭轮廓时给 draw_polyline 的 closed 传 true。
     * 示例：
     * @code
     * if (const auto *points = ctx->roi_polygon_at(0))
     * {
     *     draw_polyline(ctx, *points, cv::Scalar(0, 255, 0), 2, 1.0, true);
     * }
     * @endcode
     */
    const std::vector<cv::Point> *roi_polygon_at(int idx) const;

    /** @brief 用网页上填写的区域名称查找关注区域，避免记区域编号。
     * @param name 网页上的区域名称，例如 "entrance"；大小写必须完全一致。
     * @return 返回第一个同名区域的地址；没找到或传 nullptr 时返回 nullptr。
     * 返回内容只在当前处理函数中读取；不要修改或保存地址到下次使用。
     * 示例：
     * @code
     * if (const RoiZone *zone = ctx->roi_by_name("entrance"))
     * {
     *     draw_poly_filled(ctx, zone->polygon);
     * }
     * @endcode
     */
    const RoiZone *roi_by_name(const char *name) const;

    /** @brief 把当前画面的时间转成可读文字，适合显示在画面上或记录日志。
     * @return 返回类似 "2026-11-16 14:30:05" 的文字，使用设备设置的本地时区。
     * 不需要传入参数；时间来自 ctx->unix_ms，离线测试时需要自己提供正确时间。
     * 示例：
     * @code
     * std::string when = ctx->time_str();
     * draw_text(ctx, when.c_str(), {20, 40});
     * @endcode
     */
    std::string time_str() const; /* "YYYY-MM-DD HH:MM:SS" —— 上报/记录用 */

    /** @brief 取得指定通道最近提供的一份图片和检测结果，例如给当前判断留一份证据。
     * @param configuredId 配置中的通道 ID；当前通道可填 ctx->chnId，不是画面排列的第几个。
     * @param out 用于接收结果的变量地址；图片在 frame 中，检测结果在 results 中。
     * @return true 表示取到通道信息，但不保证有图，还要检查 frame.empty()；通道不存在等情况返回 false。
     * 需要视频程序已启动；图片和检测结果会复制给你，不会被后续新画面改掉。
     * 取到的是该通道最近提供的数据，不保证与当前通道同一时刻；只看当前图用 business_frame()。
     * 示例：
     * @code
     * ChannelFrameSnapshot snapshot;
     * if (ctx->get_channel_frame_snapshot(2, &snapshot) && !snapshot.frame.empty())
     * {
     *     cv::Mat evidence = snapshot.frame; // 这一份图片可以留到函数返回以后使用
     * }
     * @endcode
     */
    bool get_channel_frame_snapshot(int configuredId, ChannelFrameSnapshot *out) const;
};

/*======================== 判断目标是否进入网页关注区域 ========================*/
/** @brief 判断一个检测框的中心点有没有进入关注区域，适合做越界或入侵判断。
 * @param ctx 程序传入的当前通道信息。
 * @param box 检测框的位置和宽高，按 640×640 图片填写。
 * @param idx 区域编号；ROI_FRAME 表示整图，ROI_ALL 表示任一区域（没有区域时也按整图）。
 * @return 中心在区域内或边界上返回 1；在外面、指定区域不存在或不足三点、ctx 为空时返回 0。
 * 本函数只判断位置，不判断模型是否运行成功。
 * 示例：
 * @code
 * int region = roi_find(ctx, "entrance");
 * cv::Rect box(100, 100, 40, 80);
 * if (roi_contains(ctx, box, region) == 1)
 * {
 *     draw_rect(ctx, box, cv::Scalar(0, 0, 255));
 * }
 * @endcode
 */
int roi_contains(const ChannelContext *ctx, const cv::Rect &box, int idx);

/** @brief 把网页区域名称换成编号，给 query_targets() 或 roi_contains() 使用。
 * @param ctx 程序传入的当前通道信息。
 * @param name 网页上填写的区域名称，大小写要一致。
 * @return 找到时返回从 0 开始的编号；没找到或参数为空时返回 ROI_NONE。
 * ROI_NONE 表示没找到区域，不能当成整张图。
 * 示例：
 * @code
 * int region = roi_find(ctx, "entrance");
 * if (region != ROI_NONE)
 * {
 *     TargetQuery query;
 *     query.roi = region;
 *     auto selected = ctx->query_targets(query);
 * }
 * @endcode
 */
int roi_find(const ChannelContext *ctx, const char *name);

/*======================== 逻辑注册 ========================*/
/** @brief 登记你写的通道处理函数，让程序能够按名称调用它。
 * @param name 配置里使用的逻辑名称，通常与函数名相同。
 * @param func 你的函数名，写法应为 void count_people(ChannelContext *ctx)。
 * @return 没有返回值；同名注册会替换原函数，空名称或空函数会被忽略。
 * 只在开始处理视频前调用。写普通逻辑模块时，通常在函数外用 REGISTER_LOGIC 即可。
 * 示例：
 * @code
 * register_logic("count_people", count_people);
 * @endcode
 */
void register_logic(const char *name, ChannelLogicFunc func);
/** @brief 给某个通道逻辑增加按钮或控制命令处理函数。
 * @param name 要接收命令的逻辑名称，必须与已注册的通道逻辑名称一致。
 * @param func 你的命令函数名；接收 ChannelContext*、const LogicAction*，返回 LogicActionResult。
 * @return 没有返回值；同名再次注册会替换原命令函数，空名称或空函数会被忽略。
 * 只在视频处理开始前调用；普通模块通常使用 REGISTER_LOGIC_ACTION。
 * 示例：
 * @code
 * register_logic_action("count_people", reset_count);
 * @endcode
 */
void register_logic_action(const char *name, ChannelLogicActionFunc func);

/*======================== 自注册辅助 (推荐用法) ========================*/
struct LogicRegistrar
{
    /** @brief 创建这个对象时就把通道函数登记好；REGISTER_LOGIC 已经替你完成此事。
     * @param name 配置中使用的逻辑名称。
     * @param func 你写的通道处理函数名。
     * @return 创建一个登记对象；以后选择该名称的逻辑时，程序会调用 func。
     * 通常无需手动创建本类对象。
     * 示例：
     * @code
     * REGISTER_LOGIC(count_people); // 放在 count_people 函数定义之后、函数体外
     * @endcode
     */
    LogicRegistrar(const char *name, ChannelLogicFunc func)
    {
        register_logic(name, func);
    }
};
/** @brief 把你写的函数登记为通道逻辑；函数写好后，在函数体外加这一行。
 * @param func 通道处理函数名，不加引号或括号；函数接收 ChannelContext*，没有返回值。
 * @return 没有返回值；函数名同时成为配置中的 logic 名称，例如 count_people。
 * 独立测试中没有可调参数时，只需这一行，配置里的 logic_parameters 填 {}。
 * 网页中模块的说明和可调参数仍写在该模块的 logic.json 中。
 * 示例：
 * @code
 * void count_people(ChannelContext *ctx)
 * {
 *     draw_text(ctx, "运行中", {20, 40});
 * }
 * REGISTER_LOGIC(count_people);
 * @endcode
 */
#define REGISTER_LOGIC(func) static const LogicRegistrar _logic_reg_##func(#func, func)

struct LogicActionRegistrar
{
    /** @brief 创建时登记一个命令处理函数；通常让 REGISTER_LOGIC_ACTION 自动完成。
     * @param name 对应的通道逻辑名称。
     * @param func 你的命令处理函数名，返回 LogicActionResult。
     * @return 创建登记对象；该逻辑收到命令后，会调用 func。
     * 示例：
     * @code
     * REGISTER_LOGIC_ACTION(count_people, reset_count); // 放在函数体外
     * @endcode
     */
    LogicActionRegistrar(const char *name, ChannelLogicActionFunc func)
    {
        register_logic_action(name, func);
    }
};
/** @brief 让某个通道逻辑可以处理按钮或控制命令，例如清零、复位。
 * @param logic_func 已用 REGISTER_LOGIC 注册的通道函数名，不加引号。
 * @param func 命令函数名；参数为 ChannelContext* 和 const LogicAction*，返回 LogicActionResult。
 * @return 没有返回值；程序把发给 logic_func 的命令交给 func 处理。
 * 写在两个函数定义之后、函数体外；func 应先检查 action，再根据 action->name 处理。
 * 示例：
 * @code
 * REGISTER_LOGIC_ACTION(count_people, reset_count);
 * @endcode
 */
#define REGISTER_LOGIC_ACTION(logic_func, func)                                                                        \
    static const LogicActionRegistrar _logic_action_reg_##func(#logic_func, func)
