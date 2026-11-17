# Reusable inference and image overlays. No video pipeline or application globals.
add_library(vision_inference STATIC
    src/vision/inference/frame_executor.cpp
    src/vision/inference/image_session.cpp
    src/vision/models/model_registry.cpp
    src/media/display/result_renderer.cpp
    src/media/display/mask_blend.cpp
    src/media/display/text_overlay.cpp
    src/pipeline/frame_transform.cpp
    src/pipeline/image_convert.cpp
    src/vision/inference/inference_result_mapping.cpp
    src/vision/inference/inference_roi_filter.cpp
    src/vision/inference/inference_roi_geometry.cpp
    src/vision/models/model_group.cpp
    src/vision/models/yolo/yolo.cpp
    src/vision/models/yolo/yolo26depth.cpp
    src/vision/models/yolo/yolo26pose.cpp
    src/vision/models/yolo/yolopose.cpp
    src/vision/models/yolo/yoloseg.cpp
    src/vision/models/yolo/yolov8det.cpp
    src/vision/models/zip-depth/zip_depth.cpp
)
target_compile_features(vision_inference PUBLIC cxx_std_14)
target_include_directories(vision_inference PUBLIC ${PROJECT_SOURCE_DIR}/include ${OpenCV_INCLUDE_DIRS}
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(vision_inference PUBLIC Vision::ModelInspection ${OpenCV_LIBS} Threads::Threads PRIVATE rga rknnrt_vendor)

add_library(Vision::Inference ALIAS vision_inference)

# Console-owned process: shared inference SDK and the same metadata probe as the application.
add_executable(image_inference_runner
    src/application/tools/image_inference_main.cpp
    src/application/tools/image_inference_cli.cpp
    src/application/tools/model_inspection_cli.cpp
)
target_include_directories(image_inference_runner PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(image_inference_runner PRIVATE Vision::Inference vision_json)
set_target_properties(image_inference_runner PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "$ORIGIN/libs"
)

option(BUILD_IMAGE_INFERENCE_EXAMPLE "Build the standalone public SDK example" OFF)
if(BUILD_IMAGE_INFERENCE_EXAMPLE)
    add_executable(image_inference_example sdk_standalone_tests/image_inference/main.cpp)
    target_link_libraries(image_inference_example PRIVATE Vision::Inference vision_json)
endif()
