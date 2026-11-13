#include "event_storage.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace event_storage
{
namespace
{
bool sync_path(const std::string &path, int flags)
{
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | flags);
    if (fd < 0)
        return false;
    const int result = fsync(fd);
    const int saved = errno;
    const int closed = close(fd);
    if (result != 0)
        errno = saved;
    return result == 0 && closed == 0;
}
} // namespace

bool sync_parent(const std::string &path)
{
    const auto slash = path.find_last_of('/');
    return sync_path(slash == std::string::npos ? "." : slash == 0 ? "/" : path.substr(0, slash), O_DIRECTORY);
}

bool sync_file(const std::string &path)
{
    return sync_path(path, O_NOFOLLOW) && sync_parent(path);
}

bool write_atomic(const std::string &path, const char *data, size_t size)
{
    std::string pattern = path + ".tmp.XXXXXX";
    std::vector<char> temporary(pattern.begin(), pattern.end());
    temporary.push_back('\0');
    const int fd = mkstemp(temporary.data());
    if (fd < 0)
        return false;
    FILE *file = fdopen(fd, "wb");
    if (!file)
    {
        const int saved = errno;
        close(fd);
        unlink(temporary.data());
        errno = saved;
        return false;
    }
    bool ok = fwrite(data, 1, size, file) == size;
    if (ok)
        ok = fflush(file) == 0;
    if (ok)
        ok = fsync(fileno(file)) == 0;
    int saved = errno;
    if (fclose(file) != 0 && ok)
    {
        ok = false;
        saved = errno;
    }
    if (ok && rename(temporary.data(), path.c_str()) != 0)
    {
        ok = false;
        saved = errno;
    }
    if (!ok)
    {
        unlink(temporary.data());
        errno = saved;
        return false;
    }
    return sync_parent(path);
}
} // namespace event_storage
