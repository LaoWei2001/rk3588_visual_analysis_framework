option(BUILD_RECOVERY_TESTS "Build service watchdog regression tests" OFF)
if(BUILD_RECOVERY_TESTS)
    enable_testing()
    add_executable(service_watchdog_tests tests/runtime/test_service_watchdog.cpp src/runtime/service_watchdog.cpp)
    target_include_directories(service_watchdog_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(service_watchdog_tests PRIVATE Threads::Threads)
    target_compile_options(service_watchdog_tests PRIVATE -UNDEBUG)
    add_test(NAME service_watchdog_regression COMMAND service_watchdog_tests)
endif()

# 推流回归测试使用真实 GStreamer 队列验证慢消费者，不依赖摄像头或 NPU。
option(BUILD_RTSP_TESTS "Build live preview backpressure regression tests" OFF)
if(BUILD_RTSP_TESTS)
    enable_testing()
    add_executable(rtsp_live_source_tests tests/rtsp/test_live_source.cpp)
    target_include_directories(rtsp_live_source_tests PRIVATE ${custom_inc} ${api_inc})
    target_compile_options(rtsp_live_source_tests PRIVATE -UNDEBUG)
    target_link_libraries(rtsp_live_source_tests PRIVATE ${GSTOPT_LIBS} Threads::Threads)
    add_test(NAME rtsp_live_source_regression COMMAND rtsp_live_source_tests)
    set_tests_properties(rtsp_live_source_regression PROPERTIES TIMEOUT 10)
endif()

option(BUILD_PERFORMANCE_TESTS "Build standalone performance counter regression tests" OFF)
if(BUILD_PERFORMANCE_TESTS)
    enable_testing()
    add_executable(performance_metrics_tests tests/performance/test_performance_metrics.cpp)
    target_include_directories(performance_metrics_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_options(performance_metrics_tests PRIVATE -UNDEBUG)
    target_link_libraries(performance_metrics_tests PRIVATE Threads::Threads)
    add_test(NAME performance_metrics_regression COMMAND performance_metrics_tests)

    add_executable(display_metrics_tests
        tests/performance/test_display_metrics.cpp
        src/display/display_render.cpp
        src/display/display.cpp
        src/display/text_overlay.cpp
        src/runtime/pause_ctrl.cpp
        src/runtime/service_watchdog.cpp
    )
    target_include_directories(display_metrics_tests PRIVATE ${custom_inc} ${api_inc})
    target_compile_options(display_metrics_tests PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
    set_target_properties(display_metrics_tests PROPERTIES LINK_FLAGS "-Wl,--gc-sections")
    target_link_libraries(display_metrics_tests PRIVATE ${OpenCV_LIBS} ${GSTOPT_LIBS} Threads::Threads)
    add_test(NAME display_metrics_regression COMMAND display_metrics_tests)
endif()

option(BUILD_PERFORMANCE_BENCHMARKS "Build RK3588 hardware display microbenchmark" OFF)
if(BUILD_PERFORMANCE_BENCHMARKS)
    add_executable(display_performance_benchmark
        tests/performance/benchmark_display.cpp
        src/display/display_render.cpp
        src/display/display.cpp
        src/display/text_overlay.cpp
        src/runtime/pause_ctrl.cpp
        src/runtime/service_watchdog.cpp
        src/pipeline/frame_transform.cpp
        src/pipeline/image_convert.cpp
    )
    target_include_directories(display_performance_benchmark PRIVATE ${custom_inc} ${api_inc})
    target_compile_options(display_performance_benchmark PRIVATE -ffunction-sections -fdata-sections)
    set_target_properties(display_performance_benchmark PROPERTIES LINK_FLAGS "-Wl,--gc-sections")
    target_link_libraries(display_performance_benchmark PRIVATE ${OpenCV_LIBS} ${GSTOPT_LIBS} rga Threads::Threads)
endif()
