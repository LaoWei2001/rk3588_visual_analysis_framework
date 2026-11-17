#include "logic_registry.h"
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace
{
// Registered during startup, then read by workers. Each callback signature owns its registry.
template <typename Callback> class CallbackRegistry
{
  public:
    void add(const char *name, Callback callback)
    {
        if (name && *name && callback)
            callbacks_[name] = callback;
    }

    Callback get(const char *name) const
    {
        if (!name)
            return nullptr;
        const auto found = callbacks_.find(name);
        return found == callbacks_.end() ? nullptr : found->second;
    }

    std::vector<std::string> names() const
    {
        std::vector<std::string> result;
        result.reserve(callbacks_.size());
        for (const auto &entry : callbacks_)
            result.push_back(entry.first);
        return result;
    }

  private:
    std::map<std::string, Callback, std::less<>> callbacks_;
};

template <typename Callback> CallbackRegistry<Callback> &callback_registry()
{
    // Function-local construction is safe for module registrars in different translation units.
    static CallbackRegistry<Callback> registry;
    return registry;
}
} // namespace

void register_logic(const char *name, ChannelLogicFunc func)
{
    callback_registry<ChannelLogicFunc>().add(name, func);
}

ChannelLogicFunc channel_logic_get(const char *name)
{
    return callback_registry<ChannelLogicFunc>().get(name);
}

std::vector<std::string> channel_logic_names()
{
    return callback_registry<ChannelLogicFunc>().names();
}

void register_logic_action(const char *name, ChannelLogicActionFunc func)
{
    callback_registry<ChannelLogicActionFunc>().add(name, func);
}

ChannelLogicActionFunc channel_logic_action_get(const char *name)
{
    return callback_registry<ChannelLogicActionFunc>().get(name);
}

void register_global_logic(const char *name, GlobalLogicFunc func)
{
    callback_registry<GlobalLogicFunc>().add(name, func);
}

GlobalLogicFunc global_logic_get(const char *name)
{
    return callback_registry<GlobalLogicFunc>().get(name);
}

std::vector<std::string> global_logic_names()
{
    return callback_registry<GlobalLogicFunc>().names();
}

void register_global_logic_action(const char *name, GlobalLogicActionFunc func)
{
    callback_registry<GlobalLogicActionFunc>().add(name, func);
}

GlobalLogicActionFunc global_logic_action_get(const char *name)
{
    return callback_registry<GlobalLogicActionFunc>().get(name);
}
