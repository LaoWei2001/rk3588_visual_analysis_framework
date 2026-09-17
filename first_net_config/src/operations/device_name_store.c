#define _POSIX_C_SOURCE 200809L

#include "operations/device_name_store.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define HOSTS_FILE_LIMIT (1024U * 1024U)

static bool write_all(int descriptor, const char *data, size_t size)
{
    size_t written = 0;

    while (written < size)
    {
        ssize_t count = write(descriptor, data + written, size - written);

        if (count < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (count == 0)
        {
            errno = EIO;
            return false;
        }
        written += (size_t)count;
    }
    return true;
}

static bool parent_directory(const char *path, char *directory, size_t size)
{
    char *slash;

    if (!path || !directory || size == 0 || strlen(path) >= size)
    {
        errno = ENAMETOOLONG;
        return false;
    }
    snprintf(directory, size, "%s", path);
    slash = strrchr(directory, '/');
    if (!slash)
    {
        snprintf(directory, size, ".");
    }
    else if (slash == directory)
    {
        slash[1] = '\0';
    }
    else
    {
        *slash = '\0';
    }
    return true;
}

static bool atomic_write_file(const char *path, const char *data, size_t size)
{
    char temporary[PATH_MAX];
    char directory[PATH_MAX];
    struct stat existing;
    mode_t mode = 0644;
    uid_t owner = geteuid();
    gid_t group = getegid();
    int descriptor = -1;
    int directory_descriptor = -1;
    bool renamed = false;
    bool ok = false;
    int saved_errno;

    if (!path || !data ||
        snprintf(temporary, sizeof(temporary), "%s.first-net-config.XXXXXX",
                 path) >= (int)sizeof(temporary) ||
        !parent_directory(path, directory, sizeof(directory)))
    {
        if (errno == 0)
            errno = ENAMETOOLONG;
        return false;
    }
    if (lstat(path, &existing) == 0)
    {
        if (!S_ISREG(existing.st_mode))
        {
            errno = EINVAL;
            return false;
        }
        mode = existing.st_mode & 07777;
        owner = existing.st_uid;
        group = existing.st_gid;
    }
    else if (errno != ENOENT)
    {
        return false;
    }

    descriptor = mkstemp(temporary);
    if (descriptor < 0)
        return false;
    (void)fcntl(descriptor, F_SETFD, FD_CLOEXEC);
    if (fchown(descriptor, owner, group) != 0 ||
        fchmod(descriptor, mode) != 0 ||
        !write_all(descriptor, data, size) || fsync(descriptor) != 0)
        goto cleanup;
    if (close(descriptor) != 0)
    {
        descriptor = -1;
        goto cleanup;
    }
    descriptor = -1;
    if (rename(temporary, path) != 0)
        goto cleanup;
    renamed = true;
    directory_descriptor = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_descriptor < 0 || fsync(directory_descriptor) != 0)
        goto cleanup;
    ok = true;

cleanup:
    saved_errno = errno;
    if (descriptor >= 0)
        (void)close(descriptor);
    if (directory_descriptor >= 0)
        (void)close(directory_descriptor);
    if (!renamed)
        (void)unlink(temporary);
    errno = saved_errno;
    return ok;
}

static bool read_file_alloc(const char *path, char **data, size_t *size)
{
    struct stat info;
    char *buffer;
    size_t used = 0;
    int descriptor;

    if (!path || !data || !size)
    {
        errno = EINVAL;
        return false;
    }
    descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return false;
    if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size < 0 || (uintmax_t)info.st_size > HOSTS_FILE_LIMIT)
    {
        int saved_errno = errno == 0 ? EFBIG : errno;
        close(descriptor);
        errno = saved_errno;
        return false;
    }
    buffer = malloc((size_t)info.st_size + 1);
    if (!buffer)
    {
        close(descriptor);
        return false;
    }
    while (used < (size_t)info.st_size)
    {
        ssize_t count = read(descriptor, buffer + used,
                             (size_t)info.st_size - used);
        if (count < 0)
        {
            if (errno == EINTR)
                continue;
            free(buffer);
            close(descriptor);
            return false;
        }
        if (count == 0)
            break;
        used += (size_t)count;
    }
    close(descriptor);
    buffer[used] = '\0';
    *data = buffer;
    *size = used;
    return true;
}

bool device_name_read_file(const char *path, char *value, size_t value_size)
{
    char *data = NULL;
    size_t size = 0;
    size_t start = 0;
    size_t end;

    if (!value || value_size == 0 || !read_file_alloc(path, &data, &size))
        return false;
    while (start < size && isspace((unsigned char)data[start]))
        start++;
    end = size;
    while (end > start && isspace((unsigned char)data[end - 1]))
        end--;
    if (end == start || end - start >= value_size)
    {
        free(data);
        errno = end == start ? EINVAL : ENAMETOOLONG;
        return false;
    }
    memcpy(value, data + start, end - start);
    value[end - start] = '\0';
    free(data);
    return true;
}

bool device_name_write_file(const char *path, const char *device_name)
{
    char content[256];
    int count;

    if (!device_name || device_name[0] == '\0')
    {
        errno = EINVAL;
        return false;
    }
    count = snprintf(content, sizeof(content), "%s\n", device_name);
    if (count <= 0 || count >= (int)sizeof(content))
    {
        errno = ENAMETOOLONG;
        return false;
    }
    return atomic_write_file(path, content, (size_t)count);
}

static bool token_equals(const char *start, const char *end, const char *text)
{
    size_t length = (size_t)(end - start);
    return strlen(text) == length && memcmp(start, text, length) == 0;
}

bool device_name_update_hosts(const char *path, const char *device_name)
{
    char *source = NULL;
    char *output = NULL;
    size_t source_size = 0;
    size_t output_size = 0;
    size_t capacity;
    const char *cursor;
    const char *source_end;
    bool replaced = false;
    bool ok;

    if (!device_name || device_name[0] == '\0' ||
        !read_file_alloc(path, &source, &source_size))
        return false;
    capacity = source_size + strlen(device_name) + 64;
    output = malloc(capacity);
    if (!output)
    {
        free(source);
        return false;
    }

    cursor = source;
    source_end = source + source_size;
    while (cursor < source_end)
    {
        const char *line_end = memchr(cursor, '\n', (size_t)(source_end - cursor));
        const char *content_end;
        const char *address_start;
        const char *address_end;

        line_end = line_end ? line_end + 1 : source_end;
        content_end = line_end;
        if (content_end > cursor && content_end[-1] == '\n')
            content_end--;
        address_start = cursor;
        while (address_start < content_end &&
               (*address_start == ' ' || *address_start == '\t'))
            address_start++;
        address_end = address_start;
        while (address_end < content_end &&
               !isspace((unsigned char)*address_end) && *address_end != '#')
            address_end++;

        if (!replaced && token_equals(address_start, address_end, "127.0.1.1"))
        {
            const char *host_start = address_end;
            const char *host_end;
            size_t prefix_size;
            size_t suffix_size;

            while (host_start < content_end &&
                   (*host_start == ' ' || *host_start == '\t'))
                host_start++;
            host_end = host_start;
            while (host_end < content_end &&
                   !isspace((unsigned char)*host_end) && *host_end != '#')
                host_end++;
            if (host_start == content_end || *host_start == '#')
            {
                prefix_size = (size_t)(address_end - cursor);
                memcpy(output + output_size, cursor, prefix_size);
                output_size += prefix_size;
                output[output_size++] = '\t';
                memcpy(output + output_size, device_name, strlen(device_name));
                output_size += strlen(device_name);
                if (host_start < content_end && *host_start == '#')
                    output[output_size++] = ' ';
                suffix_size = (size_t)(line_end - host_start);
                memcpy(output + output_size, host_start, suffix_size);
                output_size += suffix_size;
            }
            else
            {
                prefix_size = (size_t)(host_start - cursor);
                suffix_size = (size_t)(line_end - host_end);
                memcpy(output + output_size, cursor, prefix_size);
                output_size += prefix_size;
                memcpy(output + output_size, device_name, strlen(device_name));
                output_size += strlen(device_name);
                memcpy(output + output_size, host_end, suffix_size);
                output_size += suffix_size;
            }
            replaced = true;
        }
        else
        {
            size_t line_size = (size_t)(line_end - cursor);
            memcpy(output + output_size, cursor, line_size);
            output_size += line_size;
        }
        cursor = line_end;
    }
    if (!replaced)
    {
        if (output_size > 0 && output[output_size - 1] != '\n')
            output[output_size++] = '\n';
        output_size += (size_t)snprintf(output + output_size,
                                        capacity - output_size,
                                        "127.0.1.1\t%s\n", device_name);
    }
    ok = atomic_write_file(path, output, output_size);
    free(output);
    free(source);
    return ok;
}
