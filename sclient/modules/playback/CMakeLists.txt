# playback：播放编排。当前先放断档检测与关键帧请求策略两个纯逻辑类。
#
# 收流/解码/渲染的线程编排（ReceiveLoop/DecodeLoop）等需要时再加。
add_library(sclient_playback STATIC
    src/StreamGapDetector.cpp
    src/KeyframeRequestPolicy.cpp
)
add_library(sclient::playback ALIAS sclient_playback)
target_include_directories(sclient_playback PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sclient_playback PUBLIC sclient::base)
sclient_set_warnings(sclient_playback)
