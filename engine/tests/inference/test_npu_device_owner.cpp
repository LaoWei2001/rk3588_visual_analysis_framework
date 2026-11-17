#include "vision/inference/npu_device_owner.h"
#include <cassert>
#include <sys/wait.h>

int main()
{
    char path[] = "/tmp/npu-device-owner-XXXXXX";
    const int temp = mkstemp(path);
    assert(temp >= 0);
    close(temp);
    {
        npu::DeviceOwner owner(path);
        const pid_t child = fork();
        assert(child >= 0);
        if (!child)
        {
            try
            {
                npu::DeviceOwner duplicate(path);
                _exit(1);
            }
            catch (const std::runtime_error &)
            {
                _exit(0);
            }
        }
        int status = 0;
        assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    npu::CoreLease::stop();
    {
        npu::DeviceOwner next(path);
        npu::CoreLease resumed(1); // A new session restores scheduling after a previous shutdown.
    }
    {
        npu::CoreLease active(1);
        bool rejected = false;
        try
        {
            npu::DeviceOwner while_active(path);
        }
        catch (const std::logic_error &)
        {
            rejected = true;
        }
        assert(rejected);
    }
    {
        npu::DeviceOwner after_failed_constructor(path); // Failed resume released the file lock too.
    }
    unlink(path); // test-private inode only
}
