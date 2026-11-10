# transport：媒体发送与网络反馈。
#
# 当前有：线格式定义、UDP 套接字封装、令牌桶整形、分片发送。
# 收流侧（反馈与重传）与 FEC 属于第二阶段。
add_library(sserver_transport STATIC
    src/udp/UdpSocket.cpp
    src/udp/TokenBucket.cpp
    src/udp/UdpSender.cpp
)
add_library(sserver::transport ALIAS sserver_transport)
target_include_directories(sserver_transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sserver_transport
    PUBLIC sserver::base
    PRIVATE Threads::Threads)
if(WIN32)
    # winsock 的调用需要显式链接；POSIX 上半点不用加。
    target_link_libraries(sserver_transport PRIVATE ws2_32)
endif()
sserver_set_warnings(sserver_transport)
