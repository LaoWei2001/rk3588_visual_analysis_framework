# Optional regression and hardware tests; production builds leave these disabled.
# ======================== Depth tests ========================
option(BUILD_DEPTH_TESTS "Build depth SDK and optional RK3588 hardware tests" OFF)
if(BUILD_DEPTH_TESTS)
    enable_testing()
    add_executable(depth_result_tests
        tests/inference/test_depth_results.cpp
        src/vision/inference/inference_result_mapping.cpp
        src/vision/inference/inference_roi_filter.cpp
        ${analysis_context_srcs}
        src/media/display/mask_blend.cpp)
    target_include_directories(depth_result_tests PRIVATE ${custom_inc} ${api_inc})
    target_compile_options(depth_result_tests PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
    set_target_properties(depth_result_tests PROPERTIES LINK_FLAGS "-Wl,--gc-sections")
    target_link_libraries(depth_result_tests PRIVATE ${OpenCV_LIBS} Threads::Threads)
    add_test(NAME depth_result_regression COMMAND depth_result_tests)

    add_executable(depth_runtime_tests tests/inference/test_depth_runtime.cpp src/vision/models/yolo/yolo26depth.cpp)
    target_include_directories(depth_runtime_tests PRIVATE ${custom_inc})
    target_compile_options(depth_runtime_tests PRIVATE -UNDEBUG)
    target_link_libraries(depth_runtime_tests PRIVATE ${OpenCV_LIBS} rknnrt_vendor rga Threads::Threads)
    set(DEPTH_TEST_MODEL "${VISION_REPOSITORY_ROOT}/yolo26n-depth-float.rknn" CACHE FILEPATH "Depth RKNN for hardware tests")
    set(DEPTH_TEST_IMAGE "${VISION_REPOSITORY_ROOT}/depth/assets/bus.jpg" CACHE FILEPATH "Depth test image")
    if(EXISTS "${DEPTH_TEST_MODEL}" AND EXISTS "${DEPTH_TEST_IMAGE}")
        add_test(NAME depth_runtime_regression COMMAND depth_runtime_tests "${DEPTH_TEST_MODEL}" "${DEPTH_TEST_IMAGE}")
        set_tests_properties(depth_runtime_regression PROPERTIES TIMEOUT 60 RESOURCE_LOCK npu LABELS hardware)
    endif()
endif()

# ======================== Npu tests ========================
option(BUILD_ZIP_DEPTH_TESTS "Build ZipDepth RK3588 integration tests" OFF)
option(BUILD_NPU_TESTS "Build common NPU dispatch and model input tests" OFF)
# Preserve the tests historically enabled by BUILD_ZIP_DEPTH_TESTS.
if(BUILD_NPU_TESTS OR BUILD_ZIP_DEPTH_TESTS)
    enable_testing()
    add_executable(npu_core_gate_tests tests/inference/test_npu_core_gate.cpp)
    target_include_directories(npu_core_gate_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_options(npu_core_gate_tests PRIVATE -UNDEBUG)
    target_link_libraries(npu_core_gate_tests PRIVATE Threads::Threads)
    add_test(NAME npu_core_gate_regression COMMAND npu_core_gate_tests)
    set_tests_properties(npu_core_gate_regression PROPERTIES TIMEOUT 10)
    add_executable(npu_execution_tests tests/inference/test_npu_execution.cpp
        tests/inference/test_npu_execution_peer.cpp)
    target_include_directories(npu_execution_tests PRIVATE ${PROJECT_SOURCE_DIR}/src ${RKNN_API_INCLUDE_DIR})
    target_compile_options(npu_execution_tests PRIVATE -UNDEBUG)
    target_link_libraries(npu_execution_tests PRIVATE Threads::Threads)
    add_test(NAME npu_execution_regression COMMAND npu_execution_tests)
    foreach(stage input run get release)
        add_test(NAME npu_execution_${stage}_failure COMMAND npu_execution_tests ${stage})
        set_tests_properties(npu_execution_${stage}_failure PROPERTIES TIMEOUT 10)
    endforeach()
    set_tests_properties(npu_execution_regression PROPERTIES TIMEOUT 10)
    foreach(test combinations device_owner)
        add_executable(npu_${test}_tests tests/inference/test_npu_${test}.cpp)
        target_include_directories(npu_${test}_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
        target_compile_options(npu_${test}_tests PRIVATE -UNDEBUG)
        target_link_libraries(npu_${test}_tests PRIVATE Threads::Threads)
        add_test(NAME npu_${test}_regression COMMAND npu_${test}_tests)
        set_tests_properties(npu_${test}_regression PROPERTIES TIMEOUT 20)
    endforeach()
    add_test(NAME npu_dispatch_audit COMMAND "${PYTHON3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/inference/check_npu_dispatch.py")
    add_test(NAME npu_performance_report_regression COMMAND "${PYTHON3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/inference/test_npu_performance_report.py")
    set_tests_properties(npu_performance_report_regression PROPERTIES TIMEOUT 10)
    add_executable(npu_model_inputs_tests tests/inference/test_npu_model_inputs.cpp
        src/vision/models/yolo/yolo.cpp src/vision/models/yolo/yolov8det.cpp src/vision/models/yolo/yolopose.cpp src/vision/models/yolo/yoloseg.cpp
        src/vision/models/yolo/yolo26pose.cpp src/vision/models/yolo/yolo26depth.cpp src/vision/models/zip-depth/zip_depth.cpp
        src/vision/inference/inference_roi_geometry.cpp src/vision/inference/inference_result_mapping.cpp)
    target_include_directories(npu_model_inputs_tests PRIVATE ${custom_inc} ${api_inc})
    target_compile_options(npu_model_inputs_tests PRIVATE -UNDEBUG)
    target_link_libraries(npu_model_inputs_tests PRIVATE ${OpenCV_LIBS} rknnrt_vendor rga Threads::Threads)
endif()
if(BUILD_ZIP_DEPTH_TESTS)
    enable_testing()
    add_executable(zip_depth_runtime_tests tests/inference/test_zip_depth_runtime.cpp
        src/vision/models/zip-depth/zip_depth.cpp src/vision/inference/inference_result_mapping.cpp
        src/pipeline/frame_transform.cpp src/pipeline/image_convert.cpp)
    target_include_directories(zip_depth_runtime_tests PRIVATE ${custom_inc} ${api_inc})
    target_compile_options(zip_depth_runtime_tests PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
    set_target_properties(zip_depth_runtime_tests PROPERTIES LINK_FLAGS "-Wl,--gc-sections")
    target_link_libraries(zip_depth_runtime_tests PRIVATE ${OpenCV_LIBS} ${GSTOPT_LIBS} rknnrt_vendor rga Threads::Threads)
    set(ZIP_DEPTH_TEST_MODEL "${VISION_REPOSITORY_ROOT}/projects/assets/zipdepth_npu_768x768-rk3588-fp16.rknn"
        CACHE FILEPATH "ZipDepth RKNN for hardware test")
    set(ZIP_DEPTH_TEST_IMAGE "${VISION_REPOSITORY_ROOT}/zipdepth/zipdepth-rknn/examples/sample.jpg"
        CACHE FILEPATH "ZipDepth test image")
    if(EXISTS "${ZIP_DEPTH_TEST_MODEL}")
        set(zip_depth_test_image_arg "--synthetic")
        if(EXISTS "${ZIP_DEPTH_TEST_IMAGE}")
            set(zip_depth_test_image_arg "${ZIP_DEPTH_TEST_IMAGE}")
        endif()
        add_test(NAME zip_depth_runtime_regression COMMAND zip_depth_runtime_tests
            "${ZIP_DEPTH_TEST_MODEL}" "${zip_depth_test_image_arg}")
        set_tests_properties(zip_depth_runtime_regression PROPERTIES TIMEOUT 120 RESOURCE_LOCK npu LABELS hardware)
    endif()
endif()

if(BUILD_NPU_TESTS)
    add_executable(rknn_native_runtime_tests tests/inference/test_rknn_native_runtime.cpp)
    target_include_directories(rknn_native_runtime_tests PRIVATE ${custom_inc} ${api_inc})
    target_compile_options(rknn_native_runtime_tests PRIVATE -UNDEBUG)
    target_link_libraries(rknn_native_runtime_tests PRIVATE ${OpenCV_LIBS} rknnrt_vendor rga Threads::Threads)
    add_executable(rknn_output_buffers_tests tests/inference/test_rknn_output_buffers.cpp)
    target_include_directories(rknn_output_buffers_tests PRIVATE ${custom_inc} ${api_inc})
    target_compile_options(rknn_output_buffers_tests PRIVATE -UNDEBUG)
    target_link_libraries(rknn_output_buffers_tests PRIVATE Threads::Threads)
    add_test(NAME rknn_output_buffers_regression COMMAND rknn_output_buffers_tests)
    add_executable(yolov8_output_parity_tests tests/inference/test_yolov8_output_parity.cpp
        src/vision/models/yolo/yolov8det.cpp src/vision/models/yolo/yolo.cpp src/vision/models/yolo/yolopose.cpp
        src/vision/models/yolo/yoloseg.cpp src/vision/models/yolo/yolo26depth.cpp)
    target_include_directories(yolov8_output_parity_tests PRIVATE ${custom_inc} ${api_inc} ${PROJECT_SOURCE_DIR}/src/vision/models/yolo)
    target_compile_options(yolov8_output_parity_tests PRIVATE -UNDEBUG)
    target_link_libraries(yolov8_output_parity_tests PRIVATE ${OpenCV_LIBS} rknnrt_vendor rga Threads::Threads)
    add_executable(detection_threshold_runtime_tests tests/inference/test_detection_threshold_runtime.cpp
        src/vision/models/yolo/yolov8det.cpp src/vision/models/yolo/yolo.cpp src/vision/models/yolo/yolopose.cpp
        src/vision/models/yolo/yoloseg.cpp src/vision/models/yolo/yolo26pose.cpp)
    target_include_directories(detection_threshold_runtime_tests PRIVATE ${custom_inc} ${api_inc})
    target_link_libraries(detection_threshold_runtime_tests PRIVATE ${OpenCV_LIBS} rknnrt_vendor rga Threads::Threads)
    add_executable(detection_threshold_decoder_tests tests/inference/test_detection_threshold_decoders.cpp
        src/vision/models/yolo/yolo.cpp src/vision/models/yolo/yolopose.cpp)
    target_include_directories(detection_threshold_decoder_tests PRIVATE ${custom_inc} ${api_inc})
    target_link_libraries(detection_threshold_decoder_tests PRIVATE ${OpenCV_LIBS} rknnrt_vendor rga Threads::Threads)
    add_executable(obb_runtime_tests tests/inference/test_obb_runtime.cpp
        src/vision/models/yolo/yolov8det.cpp src/vision/models/yolo/yolopose.cpp src/vision/inference/inference_result_mapping.cpp)
    target_include_directories(obb_runtime_tests PRIVATE ${custom_inc} ${api_inc})
    target_compile_options(obb_runtime_tests PRIVATE -UNDEBUG)
    target_link_libraries(obb_runtime_tests PRIVATE ${OpenCV_LIBS} rknnrt_vendor rga Threads::Threads)
    if(EXISTS "${VISION_REPOSITORY_ROOT}/projects/assets/yolov8n_obb.rknn")
        add_test(NAME obb_runtime_regression COMMAND obb_runtime_tests "${VISION_REPOSITORY_ROOT}")
        set_tests_properties(obb_runtime_regression PROPERTIES TIMEOUT 120 RESOURCE_LOCK npu LABELS hardware)
    endif()
endif()

# ======================== Sdk tests ========================
option(BUILD_SDK_TESTS "Validate public SDK without engine include paths" OFF)
if(BUILD_SDK_TESTS)
    enable_testing()
    add_executable(sdk_network_rtt_tests tests/sdk/test_network_rtt.cpp)
    target_include_directories(sdk_network_rtt_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_options(sdk_network_rtt_tests PRIVATE -UNDEBUG)
    target_link_libraries(sdk_network_rtt_tests PRIVATE Vision::Network)
    set_target_properties(sdk_network_rtt_tests PROPERTIES LINK_FLAGS "-Wl,--wrap=send")
    add_test(NAME sdk_network_rtt_regression COMMAND sdk_network_rtt_tests)
    set_tests_properties(sdk_network_rtt_regression PROPERTIES TIMEOUT 10 SKIP_RETURN_CODE 77)
    set(sdk_test_args
        --compiler "${CMAKE_CXX_COMPILER}"
        --include "${PROJECT_SOURCE_DIR}/include"
        --projects "${VISION_PROJECTS_ROOT}"
    )
    foreach(directory IN LISTS OpenCV_INCLUDE_DIRS)
        list(APPEND sdk_test_args --opencv "${directory}")
    endforeach()
    add_test(NAME sdk_public_headers_regression
        COMMAND "${PYTHON3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/sdk/test_public_headers.py" ${sdk_test_args})
    set_tests_properties(sdk_public_headers_regression PROPERTIES TIMEOUT 180)

    add_test(NAME sdk_inference_dependencies_regression
        COMMAND "${PYTHON3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/sdk/test_inference_dependencies.py"
            --source "${PROJECT_SOURCE_DIR}" --cmake "${CMAKE_COMMAND}")
    set_tests_properties(sdk_inference_dependencies_regression PROPERTIES TIMEOUT 60)

    add_executable(sdk_gpio_protocol_tests tests/sdk/test_gpio_protocol.cpp)
    target_include_directories(sdk_gpio_protocol_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_options(sdk_gpio_protocol_tests PRIVATE -UNDEBUG)
    set_target_properties(sdk_gpio_protocol_tests PROPERTIES
        LINK_FLAGS "-Wl,--wrap=gpiod_chip_open_by_name,--wrap=gpiod_chip_open_by_number")
    target_link_libraries(sdk_gpio_protocol_tests PRIVATE Vision::SDK gpiod Threads::Threads)
    add_test(NAME sdk_gpio_protocol_regression COMMAND sdk_gpio_protocol_tests)
    set_tests_properties(sdk_gpio_protocol_regression PROPERTIES TIMEOUT 15)

    add_library(sdk_target_query_consumer OBJECT tests/sdk/test_target_query.cpp)
    target_link_libraries(sdk_target_query_consumer PRIVATE Vision::SDK)
    target_compile_options(sdk_target_query_consumer PRIVATE -UNDEBUG)
    add_executable(sdk_target_query_tests
        $<TARGET_OBJECTS:sdk_target_query_consumer>
        ${analysis_context_srcs}
        src/media/display/mask_blend.cpp
    )
    target_include_directories(sdk_target_query_tests PRIVATE ${custom_inc} ${api_inc})
    target_compile_options(sdk_target_query_tests PRIVATE -ffunction-sections -fdata-sections)
    set_target_properties(sdk_target_query_tests PROPERTIES LINK_FLAGS "-Wl,--gc-sections")
    target_link_libraries(sdk_target_query_tests PRIVATE ${OpenCV_LIBS} Threads::Threads)
    add_test(NAME sdk_target_query_regression COMMAND sdk_target_query_tests)
    set_tests_properties(sdk_target_query_regression PROPERTIES TIMEOUT 15)

    add_executable(sdk_logic_session_tests tests/sdk/test_logic_session.cpp)
    target_compile_options(sdk_logic_session_tests PRIVATE -UNDEBUG)
    target_link_libraries(sdk_logic_session_tests PRIVATE Vision::Analysis)
    add_test(NAME sdk_logic_session_regression COMMAND sdk_logic_session_tests)
    set_tests_properties(sdk_logic_session_regression PROPERTIES TIMEOUT 30)

    add_executable(sdk_logic_registry_tests tests/sdk/test_logic_registry.cpp)
    target_include_directories(sdk_logic_registry_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_options(sdk_logic_registry_tests PRIVATE -UNDEBUG)
    target_link_libraries(sdk_logic_registry_tests PRIVATE Vision::Runtime)
    add_test(NAME sdk_logic_registry_regression COMMAND sdk_logic_registry_tests)

    add_executable(sdk_parameters_tests tests/sdk/test_parameters.cpp)
    target_compile_options(sdk_parameters_tests PRIVATE -UNDEBUG)
    target_link_libraries(sdk_parameters_tests PRIVATE Vision::Analysis)
    add_test(NAME sdk_parameters_regression COMMAND sdk_parameters_tests)

    add_executable(sdk_rendering_tests tests/sdk/test_rendering.cpp)
    target_compile_options(sdk_rendering_tests PRIVATE -UNDEBUG)
    target_link_libraries(sdk_rendering_tests PRIVATE Vision::Inference)
    add_test(NAME sdk_rendering_regression COMMAND sdk_rendering_tests)

    add_executable(sdk_runtime_lifecycle_tests tests/runtime/test_video_runtime.cpp)
    target_compile_options(sdk_runtime_lifecycle_tests PRIVATE -UNDEBUG)
    target_link_libraries(sdk_runtime_lifecycle_tests PRIVATE Vision::Runtime)
    add_test(NAME sdk_runtime_lifecycle_regression COMMAND sdk_runtime_lifecycle_tests)
    set_tests_properties(sdk_runtime_lifecycle_regression PROPERTIES TIMEOUT 15)

    add_executable(runtime_initialization_tests tests/runtime/test_runtime_initialization.cpp)
    target_include_directories(runtime_initialization_tests PRIVATE ${custom_inc} ${api_inc})
    target_compile_options(runtime_initialization_tests PRIVATE -UNDEBUG)
    target_link_libraries(runtime_initialization_tests PRIVATE Vision::Runtime)
    add_test(NAME runtime_initialization_regression COMMAND runtime_initialization_tests)
    set_tests_properties(runtime_initialization_regression PROPERTIES TIMEOUT 30)

    add_executable(runtime_design_tests tests/runtime/test_runtime_design.cpp)
    target_include_directories(runtime_design_tests PRIVATE ${custom_inc} ${api_inc})
    target_compile_options(runtime_design_tests PRIVATE -UNDEBUG)
    target_link_libraries(runtime_design_tests PRIVATE Vision::Runtime)
    add_test(NAME runtime_design_regression COMMAND runtime_design_tests)
    set_tests_properties(runtime_design_regression PROPERTIES TIMEOUT 30)

    add_executable(file_playback_tests tests/runtime/test_file_playback.cpp)
    target_compile_options(file_playback_tests PRIVATE -UNDEBUG)
    target_link_libraries(file_playback_tests PRIVATE Vision::Runtime)
    add_test(NAME file_playback_regression COMMAND ${PYTHON3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/runtime/run_file_playback.py $<TARGET_FILE:file_playback_tests>)
    set_tests_properties(file_playback_regression PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)

    add_executable(logic_config_tests tests/runtime/test_logic_config.cpp)
    target_include_directories(logic_config_tests PRIVATE ${custom_inc} ${api_inc})
    target_compile_options(logic_config_tests PRIVATE -UNDEBUG)
    target_link_libraries(logic_config_tests PRIVATE Vision::Runtime)
    add_test(NAME logic_config_default_regression COMMAND logic_config_tests)
    add_test(NAME logic_config_explicit_regression COMMAND logic_config_tests explicit)

    add_executable(sdk_default_parameters_tests tests/sdk/test_default_logic_parameters.cpp)
    target_compile_options(sdk_default_parameters_tests PRIVATE -UNDEBUG)
    target_link_libraries(sdk_default_parameters_tests PRIVATE Vision::Analysis)
    add_test(NAME sdk_default_parameters_regression COMMAND sdk_default_parameters_tests)
    add_test(NAME sdk_explicit_catalog_regression COMMAND sdk_default_parameters_tests explicit)

    # The example is optional in externally supplied business workspaces.
    set(sdk_example "${VISION_PROJECTS_ROOT}/modules/logic_roi_dwell_demo/logic.cpp")
    if(EXISTS "${sdk_example}")
        add_library(sdk_example_consumer OBJECT "${sdk_example}")
        target_link_libraries(sdk_example_consumer PRIVATE Vision::SDK)
        add_executable(sdk_business_tests
            tests/sdk/test_business_workflow.cpp
            $<TARGET_OBJECTS:sdk_example_consumer>
            "${logic_catalog_cpp}"
        )
        target_compile_options(sdk_business_tests PRIVATE -UNDEBUG)
        target_link_libraries(sdk_business_tests PRIVATE Vision::Analysis)
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/sdk-acceptance")
        add_test(NAME sdk_business_workflow_regression
            COMMAND sdk_business_tests "${CMAKE_CURRENT_BINARY_DIR}/sdk-acceptance")
        set_tests_properties(sdk_business_workflow_regression PROPERTIES TIMEOUT 30)
    endif()
endif()

# ======================== Runtime tests ========================
option(BUILD_RECOVERY_TESTS "Build service watchdog regression tests" OFF)
if(BUILD_RECOVERY_TESTS)
    enable_testing()
    add_executable(service_watchdog_tests tests/runtime/test_service_watchdog.cpp src/application/runtime/service_watchdog.cpp)
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
    add_test(NAME rtsp_source_preview_regression COMMAND "${PYTHON3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/rtsp/test_source_preview.py" $<TARGET_FILE:vision_analysis>)
    set_tests_properties(rtsp_source_preview_regression PROPERTIES TIMEOUT 120 LABELS hardware RESOURCE_LOCK video)
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
        src/media/display/display_render.cpp
        src/media/display/display.cpp
        src/media/display/result_renderer.cpp
        src/media/display/text_overlay.cpp
        src/application/runtime/service_watchdog.cpp
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
        src/media/display/display_render.cpp
        src/media/display/display.cpp
        src/media/display/result_renderer.cpp
        src/media/display/text_overlay.cpp
        src/application/runtime/service_watchdog.cpp
        src/pipeline/frame_transform.cpp
        src/pipeline/image_convert.cpp
    )
    target_include_directories(display_performance_benchmark PRIVATE ${custom_inc} ${api_inc})
    target_compile_options(display_performance_benchmark PRIVATE -ffunction-sections -fdata-sections)
    set_target_properties(display_performance_benchmark PROPERTIES LINK_FLAGS "-Wl,--gc-sections")
    target_link_libraries(display_performance_benchmark PRIVATE ${OpenCV_LIBS} ${GSTOPT_LIBS} rga Threads::Threads)
endif()

# ======================== Inference tests ========================
# 跟踪器回归测试不依赖 RKNN/GStreamer 运行时，默认不纳入产品构建。
option(BUILD_TRACKER_TESTS "Build standalone tracker regression tests" OFF)
if(BUILD_TRACKER_TESTS)
    enable_testing()
    add_executable(tracker_tests
        tests/tracking/test_tracker.cpp
        src/vision/tracking/tracker.cpp
    )
    target_include_directories(tracker_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${OpenCV_INCLUDE_DIRS}
        ${PROJECT_SOURCE_DIR}/src
    )
    target_link_libraries(tracker_tests PRIVATE ${OpenCV_LIBS})
    add_test(NAME tracker_regression COMMAND tracker_tests)

    add_executable(bytetrack_tests
        tests/tracking/test_bytetrack.cpp
        src/vision/tracking/bytetrack.cpp
    )
    target_include_directories(bytetrack_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${OpenCV_INCLUDE_DIRS}
        ${PROJECT_SOURCE_DIR}/src
    )
    target_link_libraries(bytetrack_tests PRIVATE ${OpenCV_LIBS})
    add_test(NAME bytetrack_regression COMMAND bytetrack_tests)
endif()

option(BUILD_INFERENCE_ROI_TESTS "Build standalone inference ROI filter tests" OFF)
option(BUILD_MODEL_METADATA_TESTS "Build CPU-only RKNN metadata and numeric validation tests" OFF)
if(BUILD_MODEL_METADATA_TESTS)
    enable_testing()
    add_executable(model_metadata_tests tests/inference/test_model_metadata.cpp)
    target_include_directories(model_metadata_tests PRIVATE src "${RKNN_API_INCLUDE_DIR}")
    target_compile_features(model_metadata_tests PRIVATE cxx_std_11)
    target_compile_options(model_metadata_tests PRIVATE -UNDEBUG)
    add_test(NAME model_metadata_regression COMMAND model_metadata_tests)
endif()
if(BUILD_INFERENCE_ROI_TESTS)
    enable_testing()
    add_executable(inference_roi_filter_tests
        tests/inference/test_inference_roi_filter.cpp
        src/vision/inference/inference_roi_filter.cpp
        src/vision/inference/inference_roi_geometry.cpp
    )
    target_include_directories(inference_roi_filter_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${OpenCV_INCLUDE_DIRS}
        ${PROJECT_SOURCE_DIR}/src
    )
    target_compile_options(inference_roi_filter_tests PRIVATE -UNDEBUG)
    target_link_libraries(inference_roi_filter_tests PRIVATE ${OpenCV_LIBS})
    add_test(NAME inference_roi_filter_regression COMMAND inference_roi_filter_tests)

    add_executable(business_coordinates_tests
        tests/inference/test_business_coordinates.cpp
        src/vision/inference/inference_result_mapping.cpp
        src/vision/inference/inference_roi_geometry.cpp
        src/vision/inference/inference_roi_filter.cpp
        src/pipeline/frame_transform.cpp
        src/pipeline/image_convert.cpp
        ${analysis_context_srcs}
        src/media/display/mask_blend.cpp
        src/media/display/display.cpp
        src/media/display/result_renderer.cpp
        src/media/display/text_overlay.cpp
    )
    target_include_directories(business_coordinates_tests PRIVATE ${custom_inc} ${api_inc} ${RKNN_API_INCLUDE_DIR})
    target_compile_options(business_coordinates_tests PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
    set_target_properties(business_coordinates_tests PROPERTIES LINK_FLAGS "-Wl,--gc-sections")
    target_link_libraries(business_coordinates_tests PRIVATE ${OpenCV_LIBS} ${GSTOPT_LIBS} rga Threads::Threads)
    add_test(NAME business_coordinates_regression COMMAND business_coordinates_tests)

    add_executable(model_status_tests
        tests/inference/test_model_status.cpp
        src/vision/inference/model_status.cpp
        src/vision/models/model_group.cpp
        ${cjson_src}
    )
    target_include_directories(model_status_tests PRIVATE ${custom_inc})
    target_compile_options(model_status_tests PRIVATE -UNDEBUG)
    target_link_libraries(model_status_tests PRIVATE ${OpenCV_LIBS} Threads::Threads)
    add_test(NAME model_status_regression COMMAND model_status_tests)
endif()

# ======================== Business tests ========================
option(BUILD_FRAME_SNAPSHOT_TESTS "Build frame evidence and buffer ownership regression tests" OFF)
if(BUILD_FRAME_SNAPSHOT_TESTS)
    enable_testing()
    add_executable(event_retention_tests tests/event/test_event_retention.cpp ${cjson_src})
    target_include_directories(event_retention_tests PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src)
    target_compile_options(event_retention_tests PRIVATE -UNDEBUG)
    add_test(NAME event_retention_regression COMMAND event_retention_tests)
    add_executable(frame_snapshot_tests
        tests/event/test_frame_snapshot.cpp
        src/application/runtime/channel_snapshot.cpp
        src/pipeline/frame_transform.cpp
        src/pipeline/image_convert.cpp
        src/media/display/display.cpp
        src/media/display/result_renderer.cpp
        src/media/display/text_overlay.cpp
    )
    target_include_directories(frame_snapshot_tests PRIVATE ${custom_inc} ${api_inc})
    target_compile_options(frame_snapshot_tests PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
    # 仅链接公共绘制函数，未使用的 GUI/实时播放入口不进入独立测试。
    set_target_properties(frame_snapshot_tests PROPERTIES LINK_FLAGS "-Wl,--gc-sections")
    target_link_libraries(frame_snapshot_tests PRIVATE ${OpenCV_LIBS} ${GSTOPT_LIBS} rga Threads::Threads)
    add_test(NAME frame_snapshot_regression COMMAND frame_snapshot_tests)
endif()

option(BUILD_DATASET_TESTS "Build dataset rule and remote collection regression tests" OFF)
if(BUILD_DATASET_TESTS)
    enable_testing()
    add_executable(dataset_rules_tests
        tests/dataset/test_rules.cpp
        src/dataset/rules.cpp
        ${cjson_src}
    )
    target_include_directories(dataset_rules_tests PRIVATE ${custom_inc} ${api_inc})
    target_compile_options(dataset_rules_tests PRIVATE -UNDEBUG)
    target_link_libraries(dataset_rules_tests PRIVATE ${OpenCV_LIBS} Threads::Threads)
    add_test(NAME dataset_rules_regression COMMAND dataset_rules_tests)
    add_executable(remote_dataset_tests tests/dataset/test_remote_dataset.cpp
        src/application/control/remote_dataset.cpp
        src/dataset/rules.cpp ${cjson_src})
    target_include_directories(remote_dataset_tests PRIVATE ${custom_inc} ${api_inc})
    target_compile_options(remote_dataset_tests PRIVATE -UNDEBUG)
    target_link_libraries(remote_dataset_tests PRIVATE ${OpenCV_LIBS} Threads::Threads)
    add_test(NAME remote_dataset_regression COMMAND remote_dataset_tests)
    set_tests_properties(remote_dataset_regression PROPERTIES TIMEOUT 20)
endif()

option(BUILD_CRANE_SAFETY_TESTS "Build crane horn GPIO timing regression tests" OFF)
if(BUILD_CRANE_SAFETY_TESTS)
    enable_testing()
    add_executable(crane_safety_tests
        tests/crane/test_safety_controller.cpp
        ${VISION_PROJECTS_ROOT}/global_modules/global_crane_safety_controller/safety_controller.cpp
    )
    target_include_directories(crane_safety_tests PRIVATE "${PROJECT_SOURCE_DIR}/include"
        ${VISION_PROJECTS_ROOT}/global_modules/global_crane_safety_controller)
    target_compile_options(crane_safety_tests PRIVATE -UNDEBUG)
    add_test(NAME crane_safety_regression COMMAND crane_safety_tests)
endif()

option(BUILD_CRANE_HOOK_TESTS "Build hook clipping, calibration and alarm regression tests" OFF)
if(BUILD_CRANE_HOOK_TESTS)
    enable_testing()
    add_executable(crane_hook_tests
        tests/crane/test_hook_geometry.cpp
        ${VISION_PROJECTS_ROOT}/modules/logic_crane_hook/hook_geometry.cpp
        ${VISION_PROJECTS_ROOT}/modules/logic_crane_hook/hook_guard.cpp
    )
    target_include_directories(crane_hook_tests PRIVATE ${custom_inc}
        ${VISION_PROJECTS_ROOT}/modules/logic_crane_hook)
    target_link_libraries(crane_hook_tests PRIVATE ${OpenCV_LIBS})
    target_compile_options(crane_hook_tests PRIVATE -UNDEBUG)
    add_test(NAME crane_hook_regression COMMAND crane_hook_tests)
endif()

option(BUILD_HELMET_TESTS "Build helmet evidence and alarm mode regression tests" OFF)
if(BUILD_HELMET_TESTS)
    enable_testing()
    add_executable(helmet_tests
        tests/helmet/test_helmet_guard.cpp
        ${VISION_PROJECTS_ROOT}/modules/logic_crane_helmet/helmet_guard.cpp
    )
    target_include_directories(helmet_tests PRIVATE ${custom_inc} ${api_inc}
        ${VISION_PROJECTS_ROOT}/modules/logic_crane_helmet)
    target_compile_options(helmet_tests PRIVATE -UNDEBUG)
    target_link_libraries(helmet_tests PRIVATE ${OpenCV_LIBS})
    add_test(NAME helmet_regression COMMAND helmet_tests)
endif()

option(BUILD_PPE_TESTS "Build PPE guard part-state and evidence window regression tests" OFF)
if(BUILD_PPE_TESTS)
    enable_testing()
    add_executable(ppe_tests
        tests/ppe/test_ppe_guard.cpp
        ${VISION_PROJECTS_ROOT}/modules/logic_ppe_guard/ppe_guard.cpp
    )
    target_include_directories(ppe_tests PRIVATE ${custom_inc} ${api_inc}
        ${VISION_PROJECTS_ROOT}/modules/logic_ppe_guard)
    target_compile_options(ppe_tests PRIVATE -UNDEBUG)
    target_link_libraries(ppe_tests PRIVATE ${OpenCV_LIBS})
    add_test(NAME ppe_regression COMMAND ppe_tests)
endif()

# Shared image/video executor contract; no real RKNN model is required.
option(BUILD_IMAGE_INFERENCE_TESTS "Build shared single-frame inference tests" OFF)
if(BUILD_IMAGE_INFERENCE_TESTS)
    enable_testing()
    add_executable(frame_executor_tests tests/inference/test_frame_executor.cpp)
    target_include_directories(frame_executor_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_options(frame_executor_tests PRIVATE -UNDEBUG)
    target_link_libraries(frame_executor_tests PRIVATE vision_inference ${CMAKE_DL_LIBS})
    add_test(NAME frame_executor_regression COMMAND frame_executor_tests)
    set_tests_properties(frame_executor_regression PROPERTIES TIMEOUT 30)
endif()
