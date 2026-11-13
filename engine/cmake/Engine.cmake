# Private engine sources; keep new implementation files registered here.
set(dir_srcs
    src/application/config/config.cpp
    src/application/config/config_init.cpp
    src/application/config/config_registry.cpp
    src/application/config/config_validator.cpp
    src/application/control/file_playback_control.cpp
    src/application/control/logic_control.cpp
    src/application/control/remote_dataset.cpp
    src/application/main.cpp
    src/application/runtime/app_ctrl.cpp
    src/application/runtime/channel_snapshot.cpp
    src/application/runtime/pause_ctrl.cpp
    src/application/runtime/publication_signal.cpp
    src/application/runtime/service_watchdog.cpp
    src/application/runtime/shutdown_status.cpp
    src/dataset/rules.cpp
    src/events/event_report.cpp
    src/events/event_storage.cpp
    src/logic/channel_logic.cpp
    src/logic/global_logic.cpp
    src/logic/logic_parameters.cpp
    src/media/capture/decode_channel.cpp
    src/media/display/display.cpp
    src/media/display/display_render.cpp
    src/media/display/text_overlay.cpp
    src/media/recorder/event_video_recorder.cpp
    src/media/rtsp/rtsp_streamer.cpp
    src/pipeline/channel_pipeline.cpp
    src/pipeline/display_worker.cpp
    src/pipeline/frame_inlet.cpp
    src/pipeline/frame_transform.cpp
    src/pipeline/image_convert.cpp
    src/pipeline/pipeline_runtime.cpp
    src/pipeline/result_dispatch.cpp
    src/pipeline/traditional_logic_worker.cpp
    src/platform/gpio/gpio.cpp
    src/vision/inference/inference_engine.cpp
    src/vision/inference/inference_executor.cpp
    src/vision/inference/inference_result_mapping.cpp
    src/vision/inference/inference_roi_filter.cpp
    src/vision/inference/inference_roi_geometry.cpp
    src/vision/inference/model_status.cpp
    src/vision/models/composite_model.cpp
    src/vision/models/model_inspection.cpp
    src/vision/models/yolo/yolo.cpp
    src/vision/models/yolo/yolo26depth.cpp
    src/vision/models/yolo/yolo26pose.cpp
    src/vision/models/yolo/yolopose.cpp
    src/vision/models/yolo/yoloseg.cpp
    src/vision/models/yolo/yolov8det.cpp
    src/vision/models/zip-depth/zip_depth.cpp
    src/vision/tracking/bytetrack.cpp
    src/vision/tracking/tracker.cpp
    "${logic_catalog_cpp}"
)

add_executable(vision_analysis ${dir_srcs} ${api_srcs} ${cjson_src})
if(TARGET vision_business)
    target_sources(vision_analysis PRIVATE $<TARGET_OBJECTS:vision_business>)
endif()
target_include_directories(vision_analysis PRIVATE ${custom_inc} ${api_inc})
target_link_libraries(vision_analysis ${custom_libs} ${sysLib_list})


add_custom_target(npu_dispatch_audit
    COMMAND "${PYTHON3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/inference/check_npu_dispatch.py")
add_dependencies(vision_analysis npu_dispatch_audit)

# 发布包即使不经过 run.sh 启动，也优先从可执行文件旁的 libs/ 加载依赖。
# BUILD_WITH_INSTALL_RPATH 避免把构建机上的 vendor 绝对路径写入最终 ELF。
set_target_properties(vision_analysis PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "$ORIGIN/libs"
)

target_compile_features(vision_analysis PRIVATE cxx_std_14)
