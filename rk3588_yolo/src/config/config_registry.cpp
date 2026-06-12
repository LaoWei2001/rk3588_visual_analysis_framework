/**
 * @file config_registry.cpp
 * @brief 统一配置解析实现
 */
#include "config_registry.h"

ConfigRegistry g_cfg_reg;

void ConfigRegistry::add_global(const char* key, ConfigType type, size_t offset) {
    global_fields.emplace_back(key, type, offset);
}

void ConfigRegistry::add_channel(const char* key, ConfigType type, size_t offset) {
    channel_fields.emplace_back(key, type, offset);
}

bool ConfigRegistry::parse_field(cJSON* obj, const ConfigField& field, void* base) const {
    if (!base) return false;

    cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, field.key);
    if (!item) return true;

    void* ptr = (void*)((char*)base + field.offset);

    switch (field.type) {
        case ConfigType::STRING:
            if (cJSON_IsString(item) && item->valuestring)
                *(std::string*)ptr = item->valuestring;
            break;
        case ConfigType::INT:
            if (cJSON_IsNumber(item))
                *(int*)ptr = item->valueint;
            break;
        case ConfigType::FLOAT:
            if (cJSON_IsNumber(item))
                *(float*)ptr = (float)item->valuedouble;
            break;
        case ConfigType::BOOL:
            if (cJSON_IsBool(item))
                *(bool*)ptr = cJSON_IsTrue(item);
            else if (cJSON_IsNumber(item))
                *(bool*)ptr = (item->valueint != 0);
            break;
        case ConfigType::STRING_ARRAY:
            if (cJSON_IsArray(item)) {
                auto& vec = *(std::vector<std::string>*)ptr;
                vec.clear();
                cJSON* elem = nullptr;
                cJSON_ArrayForEach(elem, item) {
                    if (cJSON_IsString(elem) && elem->valuestring)
                        vec.push_back(elem->valuestring);
                }
            }
            break;
    }
    return true;
}

bool ConfigRegistry::parse_global(cJSON* obj, void* base) {
    if (!cJSON_IsObject(obj) || !base) return false;
    for (const auto& f : global_fields) parse_field(obj, f, base);
    return true;
}

bool ConfigRegistry::parse_channel(cJSON* obj, void* base) {
    if (!cJSON_IsObject(obj) || !base) return false;
    for (const auto& f : channel_fields) parse_field(obj, f, base);
    return true;
}

bool ConfigRegistry::sync_fields(void* dst, const void* src, bool is_global) const {
    if (!dst || !src) return false;

    const auto& fields = is_global ? global_fields : channel_fields;
    for (const auto& field : fields) {
        char* dst_ptr = static_cast<char*>(dst) + field.offset;
        const char* src_ptr = static_cast<const char*>(src) + field.offset;

        switch (field.type) {
            case ConfigType::STRING:
                *reinterpret_cast<std::string*>(dst_ptr) = *reinterpret_cast<const std::string*>(src_ptr);
                break;
            case ConfigType::INT:
                *reinterpret_cast<int*>(dst_ptr) = *reinterpret_cast<const int*>(src_ptr);
                break;
            case ConfigType::FLOAT:
                *reinterpret_cast<float*>(dst_ptr) = *reinterpret_cast<const float*>(src_ptr);
                break;
            case ConfigType::BOOL:
                *reinterpret_cast<bool*>(dst_ptr) = *reinterpret_cast<const bool*>(src_ptr);
                break;
            case ConfigType::STRING_ARRAY:
                *reinterpret_cast<std::vector<std::string>*>(dst_ptr) =
                    *reinterpret_cast<const std::vector<std::string>*>(src_ptr);
                break;
        }
    }

    return true;
}
