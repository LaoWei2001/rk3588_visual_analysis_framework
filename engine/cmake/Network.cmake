# ICMP measurement is usable without video, OpenCV or an application context.
add_library(vision_network STATIC src/media/capture/network_rtt.cpp)
target_include_directories(vision_network PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(vision_network PUBLIC cxx_std_11)
target_link_libraries(vision_network PUBLIC Threads::Threads)
add_library(Vision::Network ALIAS vision_network)
