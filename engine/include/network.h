#pragma once

#include <string>

namespace vision
{
struct NetworkRttResult
{
    float milliseconds = -1; ///< 成功时为往返毫秒数；失败时为 -1。
    std::string error;       ///< 失败原因；成功时为空。
};

/** @brief 向摄像头发送一次 ICMP Echo，测量网络往返延迟，不启动视频或模型。
 * @param ip 摄像头的 IPv4/IPv6 地址；不填 RTSP 地址或域名。IPv6 可附带 %网卡名。
 * @param timeout_ms 等待回复的最长时间（毫秒，必须 >0）；调用期间会等待。
 * @return milliseconds >=0 表示成功，否则查看 error；不响应 ICMP 不代表视频一定断流。
 * @note 同步测量请放在测试程序或后台线程，避免在每帧业务/绘图回调中调用。
 * 示例：
 * @code
 * const auto result = vision::measure_network_rtt("192.168.1.64");
 * if (result.milliseconds >= 0)
 *     std::cout << result.milliseconds << " ms\n";
 * else
 *     std::cerr << result.error << '\n';
 * @endcode
 */
NetworkRttResult measure_network_rtt(const std::string &ip, int timeout_ms = 1000);
} // namespace vision
