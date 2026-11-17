# Public headers never expose private engine or vendor include directories.
add_library(vision_sdk INTERFACE)
add_library(Vision::SDK ALIAS vision_sdk)
target_include_directories(vision_sdk INTERFACE ${PROJECT_SOURCE_DIR}/include)
target_include_directories(vision_sdk SYSTEM INTERFACE ${OpenCV_INCLUDE_DIRS})
target_compile_features(vision_sdk INTERFACE cxx_std_11)

add_library(vision_json STATIC ${cjson_src})
target_include_directories(vision_json PRIVATE ${PROJECT_SOURCE_DIR}/include)
