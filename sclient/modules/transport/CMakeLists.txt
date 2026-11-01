# transport：收流与弱网恢复。
#
# 目前只有抖动缓冲的纯策略部分（不持有 socket、不依赖 Qt），以 INTERFACE
# 库导出。加入 StreamClient 实现后改为 STATIC，并接入 Qt Network 与
# mediasec（AES-GCM）。
add_library(sclient_transport INTERFACE)
add_library(sclient::transport ALIAS sclient_transport)
target_include_directories(sclient_transport INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sclient_transport INTERFACE sclient::base)
