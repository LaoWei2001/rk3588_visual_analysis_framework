/**
 * @file constants.h
 * @brief 系统常量定义
 */
#pragma once
namespace constants
{

// 实时预览：按源帧自动更新，超过此速率时丢弃旧帧保护 RGA/内存带宽
constexpr int PREVIEW_MAX_FPS = 30;

} // namespace constants
