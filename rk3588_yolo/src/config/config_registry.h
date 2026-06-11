/**
 * @file config_registry.h
 * @brief 统一配置解析系统
 */
#pragma once

#include <string>
#include <cstddef>
#include <functional>
#include <vector>
#include "../third_party/json/cJSON.h"

enum class ConfigType { STRING, INT, FLOAT, BOOL, STRING_ARRAY };

struct ConfigField {
    const char* key;
    ConfigType type;
    size_t offset;

    ConfigField(const char* k, ConfigType t, size_t off)
        : key(k), type(t), offset(off) {}
};

class ConfigRegistry {
public:
    void add_global(const char* key, ConfigType type, size_t offset);
    void add_channel(const char* key, ConfigType type, size_t offset);
    bool parse_global(cJSON* obj, void* base);
    bool parse_channel(cJSON* obj, void* base);
    bool sync_fields(void* dst, const void* src, bool is_global) const;
    void add_reload_callback(std::function<void()> cb) { callbacks.push_back(cb); }
    void trigger_reload() { for (auto& cb : callbacks) cb(); }

private:
    std::vector<ConfigField> global_fields;
    std::vector<ConfigField> channel_fields;
    std::vector<std::function<void()>> callbacks;
    bool parse_field(cJSON* obj, const ConfigField& field, void* base) const;
};

extern ConfigRegistry g_cfg_reg;

#define REG_G(k, t, f) g_cfg_reg.add_global(k, ConfigType::t, offsetof(AppConfig, f))
#define REG_C(k, t, f) g_cfg_reg.add_channel(k, ConfigType::t, offsetof(ChannelConfig, f))
