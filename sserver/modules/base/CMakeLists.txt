# base：媒体基础类型与通用工具（帧模型 / 时钟 / 队列 / 诊断 / 延迟统计）。
#
# 当前阶段只有头文件（帧模型与队列模板），因此以 INTERFACE 库导出；
# 加入 LatencyRecorder.cpp 等实现时改为 add_library(sserver_base STATIC ...)
# 并把 Threads 依赖从 INTERFACE 改为 PUBLIC。
add_library(sserver_base INTERFACE)
add_library(sserver::base ALIAS sserver_base)
target_include_directories(sserver_base INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sserver_base INTERFACE Threads::Threads)
