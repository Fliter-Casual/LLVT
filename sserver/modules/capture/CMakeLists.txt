# capture：V4L2 采集。
#
# 当前只有格式协商的纯决策部分（过滤/排序/对齐），不依赖 linux/videodev2.h，
# 因此没有摄像头也能单测。V4L2FrameSource 与真正的协商循环等 D7 接真机时再加。
add_library(sserver_capture STATIC
    src/FormatSelectionPolicy.cpp
)
add_library(sserver::capture ALIAS sserver_capture)
target_include_directories(sserver_capture PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sserver_capture PUBLIC sserver::base)
sserver_set_warnings(sserver_capture)