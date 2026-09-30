#pragma once

#include <cstdint>
#include <string>

/**
 * Web/外部控制统一向逻辑模块投递“动作名 + JSON 参数”。动作只会在对应逻辑的
 * 执行线程中调用，因此处理函数可以安全读写 ctx->state。
 */
struct LogicAction
{
    std::string request_id;
    std::string name;
    std::string payload_json = "{}";
    std::string logic_name; /* 入队时对应的 logic，热切换后不误投给其它 logic。 */
    uint64_t received_unix_ms = 0;
};

struct LogicActionResult
{
    bool handled = false;
    std::string message;
};
