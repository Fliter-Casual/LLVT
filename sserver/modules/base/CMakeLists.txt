# base：媒体基础类型与通用工具（帧模型 / 时钟 / 队列 / 延迟统计）。
# 
# 头文件部分（帧模型、队列模板）本身就是 INTERFACE 内容；LatencyRecorder
# 有独立的编译单元，所以这里用 STATIC 库，并把 Threads 依赖以 PUBLIC
# 导出——用了 std::mutex 的消费者也需要链接它。
add_library(sserver_base STATIC
    src/LatencyRecorder.cpp
)

add_library(sserver::base ALIAS sserver_base)
target_include_directories(sserver_base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sserver_base PUBLIC Threads::Threads)
sserver_set_warnings(sserver_base)