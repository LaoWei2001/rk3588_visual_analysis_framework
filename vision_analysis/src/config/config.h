#pragma once
#include <rkvision/config_types.h>
#include "runtime/constants.h"
namespace config_utils
{
bool starts_with(const std::string &value, const char *prefix);
std::string to_lower_copy(const std::string &value);
std::string normalize_src_type(const StreamConfig &stream);
std::string resolve_stream_location(const StreamConfig &stream, const std::string &src_type);
bool is_supported_src_type(const std::string &src_type);
bool is_channel_infer_enabled(const ChannelConfig &ch_cfg);
} // namespace config_utils

/*======================== 接口 ========================*/
/**
 * @brief 从JSON文件加载配置
 * @param path 配置文件路径
 * @param cfg  输出配置结构
 * @return true=成功, false=失败
 */
bool load_config(const std::string &path, AppConfig &cfg);

/**
 * @brief 获取配置文件的最后修改时间 (用于热加载检测)
 * @param path 文件路径
 * @return 修改时间戳, 失败返回0
 */
uint64_t config_get_mtime(const std::string &path);

/**
 * @brief 验证配置有效性
 * @param cfg 配置结构
 * @return true=有效, false=无效
 */
