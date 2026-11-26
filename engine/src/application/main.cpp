/*
 * Copyright (C) 2026 JNU IOT C301 Sunny_Wei, all rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/** @file main.cpp
 * @brief CLI 分发；视频生命周期由公开 SDK VideoRuntime 统一实现。
 */
#include <parameters.h>
#include <runtime.h>

#include "application/config/config.h"
#include "application/tools/image_inference_cli.h"
#include "application/tools/model_inspection_cli.h"
#include "logic/global_logic.h"
#include "logic/logic_registry.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <utility>

extern const char *logic_embedded_catalog_json();

int main(int argc, char **argv)
{
    /* Web 控制台通过 PIPE 捕获日志。stdout 连到管道时 libc 默认使用块缓冲，
     * 冷启动/重连日志可能长时间不显示；统一改为逐行刷新。 */
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    if (argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0))
    {
        std::printf("用法\n  %s [配置.json]\n  %s <工具选项>\n\n", argv[0], argv[0]);
        std::puts("默认配置为 ./assets/config.json。程序在前台运行，按 Ctrl+C 停止。\n\n"
                  "工具选项\n"
                  "  -h, --help                         显示本帮助\n"
                  "  --validate-config <配置.json>      校验视频配置及其引用的文件\n"
                  "  --list-logics                      列出通道级业务逻辑\n"
                  "  --list-global-logics               列出全局业务逻辑\n"
                  "  --inspect-model <模型.rknn> [类型] 查看模型输入输出，可指定模型类型检查兼容性\n"
                  "  --image-inference-capabilities     查看图片推理支持的模型类型和限制\n"
                  "  --infer-images <任务.json>         执行图片推理任务\n"
                  "  --watchdog-protocol                显示引擎支持的看门狗协议");
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "--image-inference-capabilities") == 0)
        return image_inference_capabilities_cli();
    if (argc == 3 && strcmp(argv[1], "--infer-images") == 0)
        return image_inference_cli(argv[2]);

    if (argc == 2 && strcmp(argv[1], "--list-logics") == 0)
    {
        for (const auto &name : channel_logic_names())
            printf("%s\n", name.c_str());
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--list-global-logics") == 0)
    {
        for (const auto &name : global_logic_names())
            printf("%s\n", name.c_str());
        return 0;
    }
    try
    {
        vision::set_logic_catalog(logic_embedded_catalog_json());
    }
    catch (const std::exception &error)
    {
        fprintf(stderr, "[Config] invalid compiled logic catalog: %s\n", error.what());
        return 1;
    }

    if (argc == 3 && strcmp(argv[1], "--validate-config") == 0)
    {
        AppConfig config;
        if (!load_config(argv[2], config))
            return 1;
        printf("[Config] validation passed: %s (%zu channels)\n", argv[2], config.channels.size());
        return 0;
    }

    if ((argc == 3 || argc == 4) && strcmp(argv[1], "--inspect-model") == 0)
        return inspect_model_cli(argv[2], argc == 4 ? argv[3] : "");

    if (argc == 2 && strcmp(argv[1], "--watchdog-protocol") == 0)
    {
        printf("rk3588-service-watchdog-v1\n");
        return 0;
    }

    vision::VideoRuntimeOptions options;
    options.config_path = (argc > 1) ? argv[1] : "./assets/config.json";
    options.install_signal_handlers = true;
    vision::VideoRuntime runtime(std::move(options));
    return runtime.run();
}
