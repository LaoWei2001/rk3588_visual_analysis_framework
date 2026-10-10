#pragma once

/* Internal helper for the initial immutable output snapshot. */
#include <outputs.h>

/** 所有“尚无公开变量”的发布版本共享同一个不可变空集合。 */
inline std::shared_ptr<const LogicOutputSet> empty_logic_output_snapshot()
{
    static const std::shared_ptr<const LogicOutputSet> empty = std::make_shared<const LogicOutputSet>();
    return empty;
}
