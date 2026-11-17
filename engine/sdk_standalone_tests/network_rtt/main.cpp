#include <network.h>
#include <iostream>

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::cerr << "用法：" << argv[0] << " 摄像头IP\n";
        return 2;
    }
    // 与正式视频程序共用 ICMP 测量实现；不启动视频、模型或 Web。
    const auto result = vision::measure_network_rtt(argv[1], 1000);
    if (result.milliseconds < 0)
    {
        std::cerr << "板端—摄像头往返时间不可用：" << result.error << '\n';
        return 1;
    }
    std::cout << "板端—摄像头往返：" << result.milliseconds << " 毫秒\n";
    // 需要画到图片上时，将结果交给已有的绘图接口：
    // options.show_network_rtt = true;
    // options.network_rtt_ms = result.milliseconds;
    // vision::render_overlays(image, options);
    return 0;
}
