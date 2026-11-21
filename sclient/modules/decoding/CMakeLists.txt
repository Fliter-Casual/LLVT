# decoding：FFmpeg 软解封装。
#
# FFmpeg 是可选依赖：装上就编真解码器，没有就明确失败。与 x264 同样的理由——
# 开发机与目标机的库状况可能不同，不能让构建在这种差异上翻车。
add_library(sclient_decoding STATIC
    src/VideoDecoder.cpp
)
add_library(sclient::decoding ALIAS sclient_decoding)
target_include_directories(sclient_decoding PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
sclient_set_warnings(sclient_decoding)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(AVCODEC QUIET libavcodec libavutil)
endif()
if(AVCODEC_FOUND)
    message(STATUS "decoding: 使用 FFmpeg (libavcodec ${AVCODEC_libavcodec_VERSION})")
    target_compile_definitions(sclient_decoding PRIVATE SCLIENT_HAS_FFMPEG)
    target_include_directories(sclient_decoding PRIVATE ${AVCODEC_INCLUDE_DIRS})
    target_link_directories(sclient_decoding PRIVATE ${AVCODEC_LIBRARY_DIRS})
    target_link_libraries(sclient_decoding PRIVATE ${AVCODEC_LIBRARIES})
else()
    message(STATUS "decoding: 未找到 FFmpeg —— 解码不可用（收流仍可用）")
endif()