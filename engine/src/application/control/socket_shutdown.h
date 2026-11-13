#pragma once

#include <atomic>
#include <mutex>
#include <sys/socket.h>
#include <unistd.h>

// One accepted connection per control server. Shutdown interrupts blocking I/O;
// only the serving thread closes the fd, under the same mutex, avoiding fd reuse.
class ControlConnection
{
  public:
    bool begin(int fd, const std::atomic<bool> &running)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running.load())
        {
            close(fd);
            return false;
        }
        fd_ = fd;
        return true;
    }
    void finish()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fd_ >= 0)
            close(fd_);
        fd_ = -1;
    }
    void cancel()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fd_ >= 0)
            shutdown(fd_, SHUT_RDWR);
    }

  private:
    std::mutex mutex_;
    int fd_{-1};
};
