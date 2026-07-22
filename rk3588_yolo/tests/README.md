# 测试

## 逻辑参数 Schema

`test_logic_parameters` 覆盖模块 Schema 的默认值填充、类型化读取、数值范围、未知键拒绝
以及热重载状态策略：

```bash
cd rk3588_yolo/tests/test_logic_parameters
bash build.sh
```

## 告警上报

`test_alarm_report/alarm_report_unit_test.cpp` 直接复用正式的 `src/alarm/alarm_report.cpp`，仅桩替换显示渲染与录像边界。

覆盖内容：

- 同通道、同报警类型 5 秒内合并；
- 不同报警类型不合并；
- 图片/视频单次选择覆盖；
- 动态逻辑字段和通道参数写入 manifest；
- 图片落盘和 delivery 初始状态。

按项目约定，应在 RK3588 板端执行：

```bash
cd rk3588_yolo/tests/test_alarm_report
cmake -S . -B build
cmake --build build
./build/alarm_report_unit_test
```
