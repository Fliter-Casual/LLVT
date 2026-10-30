# base：媒体基础类型与通用工具（帧模型 / 时钟 / 队列 / 诊断 / 延迟统计）。
# base 模块目前以类模板和 inline 函数为主
# 当前阶段只有头文件（帧模型与队列模板），因此以 INTERFACE 库导出；
# 加入 LatencyRecorder.cpp 等实现时改为 add_library(sserver_base STATIC ...)
# 并把 Threads 依赖从 INTERFACE 改为 PUBLIC。
# INTERFACE（纯接口库）
add_library(sserver_base INTERFACE) # 静态库
add_library(sserver::base ALIAS sserver_base) # 符号
target_include_directories(sserver_base INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include) # 头文件
target_link_libraries(sserver_base INTERFACE Threads::Threads)  # 线程库
