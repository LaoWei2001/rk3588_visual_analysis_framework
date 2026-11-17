#include <cassert>
#include <channel.h>
#include <global.h>
#include <parameters.h>
#include <stdexcept>

void channel_without_parameters(ChannelContext *)
{
}
void global_without_parameters(GlobalContext *)
{
}
REGISTER_LOGIC(channel_without_parameters);
REGISTER_GLOBAL_LOGIC(global_without_parameters);

int main(int argc, char **)
{
    const bool explicit_catalog = argc > 1;
    if (explicit_catalog)
        vision::set_logic_catalog(R"({"channel_logics":[],"global_logics":[]})");
    for (const char *name : {"channel_without_parameters", "global_without_parameters"})
    {
        assert(logic_parameters_resolve(name, "{}", nullptr, nullptr, nullptr) == !explicit_catalog);
        assert(!logic_parameters_resolve(name, R"({"typo":1})", nullptr, nullptr, nullptr));
    }
    assert(!logic_parameters_resolve("unknown_callback", "{}", nullptr, nullptr, nullptr));
    assert(logic_parameters_resolve("", "{}", nullptr, nullptr, nullptr));
    bool sealed = false;
    try
    {
        vision::set_logic_catalog(R"({"channel_logics":[],"global_logics":[]})");
    }
    catch (const std::logic_error &)
    {
        sealed = true;
    }
    assert(sealed);
}
