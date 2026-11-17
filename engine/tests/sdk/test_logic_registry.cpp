#include "logic/logic_registry.h"

#include <algorithm>
#include <cassert>
#include <iostream>

namespace
{
void channel(ChannelContext *)
{
}
void channel_replacement(ChannelContext *)
{
}
void global(GlobalContext *)
{
}
void global_replacement(GlobalContext *)
{
}
LogicActionResult channel_action(ChannelContext *, const LogicAction *)
{
    return {true, "channel"};
}
LogicActionResult global_action(GlobalContext *, const LogicAction *)
{
    return {true, "global"};
}
} // namespace

int main()
{
    register_logic(nullptr, channel);
    register_logic("", channel);
    register_logic("ignored", nullptr);
    register_logic_action("", channel_action);
    register_global_logic(nullptr, global);
    register_global_logic("", global);
    register_global_logic("ignored", nullptr);
    register_global_logic_action("", global_action);
    assert(channel_logic_get(nullptr) == nullptr && channel_logic_get("") == nullptr);
    assert(global_logic_get(nullptr) == nullptr && global_logic_get("") == nullptr);
    assert(channel_logic_get("ignored") == nullptr && global_logic_get("ignored") == nullptr);
    assert(channel_logic_action_get("") == nullptr && global_logic_action_get("") == nullptr);

    // Names are owned; the 65th and later modules must remain callable too.
    for (int i = 0; i < 100; ++i)
    {
        const std::string name = "registry_test_" + std::to_string(i);
        register_logic(name.c_str(), channel);
        register_logic_action(name.c_str(), channel_action);
        register_global_logic(name.c_str(), global);
        register_global_logic_action(name.c_str(), global_action);
    }
    for (int i = 0; i < 100; ++i)
    {
        const std::string name = "registry_test_" + std::to_string(i);
        assert(channel_logic_get(name.c_str()) == channel);
        assert(channel_logic_action_get(name.c_str()) == channel_action);
        assert(global_logic_get(name.c_str()) == global);
        assert(global_logic_action_get(name.c_str()) == global_action);
    }
    const auto channel_names = channel_logic_names();
    const auto global_names = global_logic_names();
    assert(std::is_sorted(channel_names.begin(), channel_names.end()));
    assert(std::is_sorted(global_names.begin(), global_names.end()));
    register_logic("registry_test_0", channel_replacement);
    register_global_logic("registry_test_0", global_replacement);
    assert(channel_logic_get("registry_test_0") == channel_replacement);
    assert(global_logic_get("registry_test_0") == global_replacement);
    assert(channel_logic_names() == channel_names && global_logic_names() == global_names);
    std::cout << "Callback registries: invalid names, owned names, 100 modules and replacement passed\n";
}
