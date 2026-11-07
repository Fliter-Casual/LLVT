# transport：媒体发送与网络反馈。
#
# 当前只有线格式定义（纯头文件）。UdpTransport 的实现要等 pipeline/app
# 能产出编码帧之后再落地。
add_library(sserver_transport INTERFACE)
add_library(sserver::transport ALIAS sserver_transport)
target_include_directories(sserver_transport INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sserver_transport INTERFACE sserver::base)
