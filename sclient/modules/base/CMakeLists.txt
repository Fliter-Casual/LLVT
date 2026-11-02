# base：客户端公共基础设施（帧模型 / 队列 / 邮箱 / 延迟统计）。
#
# 当前只有头文件（统计器在线性规划里是纯逻辑，无需编译单元），
# 以 INTERFACE 库导出；将来加入 DecodedFrame 的实现文件时改为 STATIC。
add_library(sclient_base INTERFACE)
add_library(sclient::base ALIAS sclient_base)
target_include_directories(sclient_base INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)