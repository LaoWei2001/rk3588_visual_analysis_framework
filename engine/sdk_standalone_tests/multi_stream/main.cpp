// 和主程序一样，通过 VideoRuntime 读取多路视频，在通道函数里取得图片。
#include <channel.h>
#include <runtime.h>

#include <cstdio>
#include <exception>

void receive_frame(ChannelContext *ctx)
{
    // 每一路都会调用这里；ctx->chnId 表示当前是哪一路。
    // frame 是原始尺寸的 BGR 图片，在本次函数调用期间使用。
    const auto *frame = ctx->source_frame();
    if (!frame || frame->empty())
        return;

    // 每路只打印一次，确认取到了图片；之后仍会继续接收图片。
    auto *printed = ctx->get_state<bool>(false);
    if (printed && !*printed)
    {
        std::printf("通道 %d：已取得图片 %d x %d\n", ctx->chnId, frame->cols, frame->rows);
        std::fflush(stdout);
        *printed = true;
    }
}
REGISTER_LOGIC(receive_frame);

int main(int argc, char **argv)
{
    using namespace vision;

    if (argc != 2)
    {
        std::fprintf(stderr, "用法：%s 配置文件.json\n", argv[0]);
        return 2;
    }

    try
    {
        // 主程序也使用这个入口：按配置打开各路视频，并调用上面的函数。
        VideoRuntimeOptions options;
        options.config_path = argv[1];
        options.install_signal_handlers = true; // Ctrl+C 停止并释放资源。
        VideoRuntime runtime(options);
        return runtime.run();
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "取流测试失败：%s\n", error.what());
        return 1;
    }
}
