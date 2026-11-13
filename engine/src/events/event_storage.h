#pragma once

#include <cstddef>
#include <string>

namespace event_storage
{
// Publish only after successful write/flush/fsync/close; sync directory metadata.
bool write_atomic(const std::string &path, const char *data, size_t size);
bool sync_file(const std::string &path);
bool sync_parent(const std::string &path);
} // namespace event_storage
