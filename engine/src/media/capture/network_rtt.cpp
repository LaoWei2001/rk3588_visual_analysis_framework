#include "network_rtt.h"
#include <cstdint>
#include <netdb.h>
#include <network.h>
#include <system_error>
#include <utility>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>

namespace
{
using Clock = std::chrono::steady_clock;
struct Socket
{
    int fd;
    ~Socket()
    {
        if (fd >= 0)
            close(fd);
    }
};

float probe(sockaddr_storage target, socklen_t length, const std::atomic<bool> &stopped, int timeout_ms,
            std::string &error)
{
    const bool ipv6 = target.ss_family == AF_INET6;
    const int protocol = ipv6 ? IPPROTO_ICMPV6 : IPPROTO_ICMP;
    bool raw = false;
    Socket socket{::socket(target.ss_family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol)};
    if (socket.fd < 0)
    {
        // Linux 可限制 ping socket；具有 CAP_NET_RAW 的宿主仍可使用标准 ICMP socket。
        raw = true;
        socket.fd = ::socket(target.ss_family, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
    }
    if (socket.fd < 0)
    {
        error = std::string("Cannot open ICMP socket: ") + std::strerror(errno);
        return -1;
    }
    if (ipv6)
        reinterpret_cast<sockaddr_in6 *>(&target)->sin6_port = 0;
    else
        reinterpret_cast<sockaddr_in *>(&target)->sin_port = 0;
    if (connect(socket.fd, reinterpret_cast<const sockaddr *>(&target), length) < 0)
    {
        error = std::string("Cannot reach target: ") + std::strerror(errno);
        return -1;
    }

    std::array<unsigned char, 24> request{};
    request[0] = ipv6 ? 128 : 8; // Echo request.
    request[4] = static_cast<unsigned char>(getpid() >> 8);
    request[5] = static_cast<unsigned char>(getpid());
    request[7] = 1;
    const auto sent = Clock::now();
    const auto token = sent.time_since_epoch().count();
    std::memcpy(request.data() + 8, &token, sizeof(token));
    const uintptr_t owner = reinterpret_cast<uintptr_t>(&stopped);
    std::memcpy(request.data() + 16, &owner, sizeof(owner));
    if (!ipv6)
    {
        unsigned checksum = 0;
        for (size_t i = 0; i < request.size(); i += 2)
            checksum += (request[i] << 8) | request[i + 1];
        while (checksum >> 16)
            checksum = (checksum & 0xffff) + (checksum >> 16);
        checksum = ~checksum;
        request[2] = static_cast<unsigned char>(checksum >> 8);
        request[3] = static_cast<unsigned char>(checksum);
    }
    // ICMPv6 的校验和由内核计算，包含 IPv6 伪首部。
    if (send(socket.fd, request.data(), request.size(), 0) != static_cast<ssize_t>(request.size()))
    {
        error = std::string("Cannot send ICMP: ") + std::strerror(errno);
        return -1;
    }
    error = "No matching ICMP reply before timeout";
    while (!stopped && Clock::now() - sent < std::chrono::milliseconds(timeout_ms))
    {
        pollfd ready{socket.fd, POLLIN, 0};
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - sent).count();
        const int wait_ms = static_cast<int>(std::max<int64_t>(1, std::min<int64_t>(50, timeout_ms - elapsed)));
        const int result = poll(&ready, 1, wait_ms);
        if (result < 0 && errno == EINTR)
            continue;
        if (result < 0)
        {
            error = std::string("Cannot wait for ICMP reply: ") + std::strerror(errno);
            return -1;
        }
        if (ready.revents & (POLLERR | POLLHUP | POLLNVAL))
        {
            int socket_error = 0;
            socklen_t error_length = sizeof(socket_error);
            getsockopt(socket.fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_length);
            error = "ICMP reply unavailable";
            if (socket_error)
                error += std::string(": ") + std::strerror(socket_error);
            return -1;
        }
        if (result == 0)
            continue;
        std::array<unsigned char, 2048> response{};
        const ssize_t received = recv(socket.fd, response.data(), response.size(), 0);
        const auto arrived = Clock::now();
        if (received < 0 && (errno == EAGAIN || errno == EINTR))
            continue;
        if (received <= 0)
        {
            error = std::string("Cannot read ICMP reply: ") + std::strerror(errno);
            return -1;
        }
        // IPv4 raw socket 包含 IP 首部；ping socket 和 IPv6 raw socket 直接返回 ICMP。
        size_t offset = 0;
        if (raw && !ipv6)
        {
            if (received < 20 || (response[0] >> 4) != 4)
                continue;
            offset = (response[0] & 15) * 4;
            if (offset < 20)
                continue;
        }
        if (static_cast<size_t>(received) != offset + request.size())
            continue;
        const auto *reply = response.data() + offset;
        // ping socket 会重写 identifier，因此用序号及本次唯一载荷匹配，忽略其他探测器的回复。
        if (reply[0] == (ipv6 ? 129 : 0) && reply[1] == 0 &&
            std::memcmp(reply + 6, request.data() + 6, request.size() - 6) == 0)
        {
            error.clear();
            return std::chrono::duration<float, std::milli>(arrived - sent).count();
        }
    }
    return -1;
}
} // namespace

vision::NetworkRttResult vision::measure_network_rtt(const std::string &ip, int timeout_ms)
{
    NetworkRttResult result;
    if (timeout_ms <= 0 || ip.empty())
    {
        result.error = "Provide an IP address and a positive timeout";
        return result;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST;
    addrinfo *addresses = nullptr;
    if (getaddrinfo(ip.c_str(), nullptr, &hints, &addresses) != 0 || !addresses)
    {
        result.error = "Invalid numeric IPv4/IPv6 address";
        return result;
    }
    sockaddr_storage target{};
    const auto length = addresses->ai_addrlen;
    std::memcpy(&target, addresses->ai_addr, length);
    freeaddrinfo(addresses);
    const std::atomic<bool> stopped{false};
    result.milliseconds = probe(target, length, stopped, timeout_ms, result.error);
    return result;
}

NetworkRttMonitor::NetworkRttMonitor(std::function<void(float)> publish) : publish_(std::move(publish))
{
}
NetworkRttMonitor::~NetworkRttMonitor()
{
    stop();
}

void NetworkRttMonitor::set_target(const sockaddr *address, socklen_t length)
{
    if (!address || (address->sa_family != AF_INET && address->sa_family != AF_INET6) || length > sizeof(target_) ||
        length < (address->sa_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6)))
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_)
        return;
    target_ = {};
    std::memcpy(&target_, address, length);
    target_length_ = length;
    ++generation_;
    publish_(-1);
    if (!thread_.joinable())
    {
        try
        {
            thread_ = std::thread(&NetworkRttMonitor::run, this);
        }
        catch (const std::system_error &)
        {
            return;
        } // 指标不可用不影响视频取流。
    }
    changed_.notify_one();
}

void NetworkRttMonitor::clear_target()
{
    std::lock_guard<std::mutex> lock(mutex_);
    target_length_ = 0;
    ++generation_;
    publish_(-1);
    changed_.notify_one();
}

void NetworkRttMonitor::stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
    }
    changed_.notify_one();
    if (thread_.joinable())
        thread_.join();
}

void NetworkRttMonitor::run()
{
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopped_)
    {
        changed_.wait(lock, [&] { return stopped_ || target_length_ != 0; });
        if (stopped_)
            break;
        const auto target = target_;
        const auto length = target_length_;
        const auto generation = generation_;
        lock.unlock();
        std::string error;
        const float rtt = probe(target, length, stopped_, 1000, error);
        lock.lock();
        if (!stopped_ && generation == generation_)
            publish_(rtt);
        changed_.wait_for(lock, std::chrono::seconds(2), [&] { return stopped_ || generation != generation_; });
    }
    publish_(-1);
}
