# Stateful frame analysis without the application, capture or output workers.
set(analysis_context_srcs
    src/logic/channel_context.cpp
    src/logic/channel_drawing.cpp
    src/logic/target_query.cpp
    src/logic/logic_registry.cpp
)
add_library(vision_analysis_sdk STATIC
    ${analysis_context_srcs}
    src/logic/logic_parameters.cpp
    src/logic/logic_session.cpp
    src/events/event_submission.cpp
    src/vision/tracking/tracker.cpp
    src/vision/tracking/bytetrack.cpp
    src/dataset/rules.cpp
)
add_library(Vision::Analysis ALIAS vision_analysis_sdk)
target_include_directories(vision_analysis_sdk PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_features(vision_analysis_sdk PUBLIC cxx_std_14)
target_link_libraries(vision_analysis_sdk PUBLIC Vision::SDK Vision::Inference PRIVATE vision_json)
