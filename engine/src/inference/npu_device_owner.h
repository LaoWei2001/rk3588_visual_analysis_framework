#pragma once

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <unistd.h>

namespace npu
{
// All framework inference programs share this lock. Never unlink its inode.
class DeviceOwner
{
  public:
    explicit DeviceOwner(const char *path = "/run/lock/rk3588-visual-npu.lock")
    {
        fd_ = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0660);
        if (fd_ < 0) throw std::runtime_error(std::string("Cannot open NPU ownership lock: ") + strerror(errno));
        if (flock(fd_, LOCK_EX | LOCK_NB) < 0)
        {
            const int error = errno;
            close(fd_); fd_ = -1;
            if (error == EWOULDBLOCK)
                throw std::runtime_error("NPU is owned by another inference process; stop it before starting this program");
            throw std::runtime_error(std::string("Cannot lock NPU device: ") + strerror(error));
        }
    }
    ~DeviceOwner() { if (fd_ >= 0) close(fd_); }
    DeviceOwner(const DeviceOwner &) = delete;
    DeviceOwner &operator=(const DeviceOwner &) = delete;
  private:
    int fd_ = -1;
};
} // namespace npu
