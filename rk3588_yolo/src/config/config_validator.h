#pragma once
#include <string>
#include <vector>
#include "config.h"

class ConfigValidator {
public:
    struct ValidationError {
        std::string field;
        std::string message;
    };

    /** 完整验证 (首次加载) */
    static bool validate(const AppConfig& cfg, std::vector<ValidationError>& errors);

    /** 分级验证 (热重载): 只检查高风险字段 (模型路径/类型/阈值范围)。
     *  非关键字段异常只输出告警, 不阻断热更新。 */
    static bool validate_critical(const AppConfig& cfg, std::vector<ValidationError>& errors);

private:
    static bool validate_global(const AppConfig& cfg, std::vector<ValidationError>& errors);
    static bool validate_channels(const AppConfig& cfg, std::vector<ValidationError>& errors);
    static bool validate_channels_critical(const AppConfig& cfg, std::vector<ValidationError>& errors);
    static bool file_exists(const std::string& path);
    static bool is_valid_url(const std::string& url);
};
