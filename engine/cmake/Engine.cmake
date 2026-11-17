# Private engine sources; keep new implementation files registered here.
set(runtime_srcs
    src/application/config/config.cpp
    src/application/config/config_init.cpp
    src/application/config/config_registry.cpp
    src/application/config/config_validator.cpp
    src/application/control/file_playback_control.cpp
    src/application/control/logic_control.cpp
    src/application/control/remote_dataset.cpp
    src/application/runtime/video_runtime.cpp
    src/application/runtime/video_runtime_startup.cpp
    src/application/runtime/video_runtime_shutdown.cpp
    src/application/runtime/video_runtime_process.cpp
    src/application/runtime/runtime_status.cpp
    src/application/runtime/app_ctrl.cpp
    src/application/runtime/channel_snapshot.cpp
    src/application/runtime/pause_ctrl.cpp
    src/application/runtime/publication_signal.cpp
    src/application/runtime/service_watchdog.cpp
    src/application/runtime/shutdown_status.cpp
    src/events/event_policy.cpp
    src/events/event_outbox.cpp
    src/events/event_images.cpp
    src/events/event_workers.cpp
    src/events/event_report.cpp
    src/events/event_storage.cpp
    src/logic/global_logic.cpp
    src/media/capture/decode_channel.cpp
    src/media/display/display.cpp
    src/media/display/display_render.cpp
    src/media/recorder/event_video_recorder.cpp
    src/media/rtsp/rtsp_streamer.cpp
    src/media/rtsp/rtsp_dma.cpp
    src/media/rtsp/rtsp_feeder.cpp
    src/media/rtsp/rtsp_preview.cpp
    src/pipeline/channel_pipeline.cpp
    src/pipeline/display_worker.cpp
    src/pipeline/frame_inlet.cpp
    src/pipeline/pipeline_runtime.cpp
    src/pipeline/result_dispatch.cpp
    src/pipeline/traditional_logic_worker.cpp
    src/platform/gpio/gpio.cpp
    src/vision/inference/inference_engine.cpp
    src/vision/inference/inference_executor.cpp
    src/vision/inference/model_status.cpp
    src/application/tools/model_inspection_cli.cpp
)

add_library(vision_runtime STATIC ${runtime_srcs} ${api_srcs})
add_library(Vision::Runtime ALIAS vision_runtime)
target_include_directories(vision_runtime PRIVATE ${custom_inc} ${api_inc})
target_compile_features(vision_runtime PUBLIC cxx_std_14)
target_link_libraries(vision_runtime PUBLIC Vision::Analysis Vision::Network PRIVATE ${custom_libs} ${sysLib_list})
