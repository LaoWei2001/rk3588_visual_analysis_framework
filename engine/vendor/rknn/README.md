# 固定的 RKNN Runtime

项目使用 `2.4.2a2/` 中的 AArch64 头文件和 `librknnrt.so` 进行编译与打包，
不从构建机的 `/usr/include`、`ldconfig` 或交叉编译 sysroot 自动选择 RKNN 版本。

这些文件按原始字节校验，调整空格或换行也会导致构建失败。本目录的 `.clang-format`
禁用格式化和 include 排序，避免编辑器保存时改写厂商头文件。请保持厂商文件原样；
不要通过修改校验值来接受格式化后的副本。

更新 Runtime 时必须同时更新以下内容：

1. 新建版本目录，不要原地覆盖旧版本；
2. 放入配套的 `rknn_api.h` 和 `librknnrt.so`；
3. 更新版本目录中的 `SHA256SUMS` 和 `RUNTIME_MANIFEST.txt`；
4. 更新 `CMakeLists.txt` 和 `build.sh` 的锁定版本及校验值；
5. 在目标 RK3588、对应 RKNPU 驱动和实际 `.rknn` 模型上完成推理验证。

发布包中的 `libs/librknnrt.manifest` 用于追溯实际打包版本。
