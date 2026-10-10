/** @file version.h
 * @brief SDK 版本和公共容量常量。
 * 功能索引与用法见同目录 README.md。
 */
#pragma once

#define VISION_SDK_VERSION_MAJOR 0
#define VISION_SDK_VERSION_MINOR 1
#define VISION_SDK_VERSION_PATCH 0
#define VISION_SDK_VERSION_STRING "0.1.0"

/* Source SDK version. Independently compiled plugin ABI is not provided. */
namespace vision
{
constexpr int MAX_CHANNELS = 15;
}
