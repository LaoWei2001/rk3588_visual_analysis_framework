# `src/third_party`：项目内嵌依赖与平台适配

该目录不是业务扩展点，但它直接决定采集、JSON 和 RK3588 平台接口的编译/运行边界。

## 子目录

| 目录 | 内容与当前用途 |
|---|---|
| `gst_opt/` | `gstopt_sample_get_buffer()`：用标准 GStreamer/GstVideo API 提取格式、可见尺寸、stride 和 DMA-BUF fd；`api.cmake` 同时声明 GStreamer、video、RTSP server、allocators 依赖。 |
| `json/` | 内嵌 cJSON 源码，供配置、control 协议和 alarm manifest 使用。 |
| `rk_mpi/` | Rockchip MPI/RK 类型头文件，`api.cmake` 链接 `rockit`；项目刻意使用目录内 `rk_type.h`，不直接取 `/usr/include/rockchip` 的同名头。 |
| `system_opt/` | CPU/NPU 温度、CPU/内存/分区统计、时间/延时、线程和命令执行辅助函数。 |

## 重要边界

`gst_opt/api.cmake` 当前编译 `gst_opt_impl.c`，不再依赖旧厂商专有 `libgst_opt.a`；目录里即使仍有该静态库文件，也不是当前 CMake 的链接真值。采集模块依赖 `FrameDesc_t` 的格式、stride 和 fd 语义，修改实现后必须同步核对 `decChannel.cpp` 和 analyzer 的 RGA 导入生命周期。

`rk_mpi` 大量文件是厂商 ABI 声明，不应为业务需求随意改结构体布局、枚举值或对齐。升级板端 `rockit`/MPP 库时，应采用匹配版本的头文件整体核验，而不是只修一个编译错误。

cJSON 对象的所有权在当前代码中依赖 `cJSON_AddItemToObject/Array` 转移；扩展配置、control 或 manifest 时必须保持 create/attach/delete 成对，字符串化结果用 `cJSON_free()`。

`system_opt` 提供 `exec_cmd_by_system/popen`，不要把未经验证的 Web/logic 输入拼入 shell 命令。新业务优先使用直接系统调用或专用库，避免把该工具层变成命令注入入口。

## 升级检查清单

1. 核对原始许可证和项目分发要求。
2. 核对头文件与板端动态/静态库 ABI、架构和版本。
3. 检查各 `api.cmake` 的 include、source 和 link 变量是否仍被顶层 CMake 使用。
4. 搜索所有调用方，尤其是 DMA-BUF 所有权、cJSON 所有权和厂商结构体布局。
5. 在目标 RK3588 设备上做专项运行验证；主机侧能通过语法检查不代表硬件路径兼容。
