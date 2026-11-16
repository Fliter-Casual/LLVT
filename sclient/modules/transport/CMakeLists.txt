# transport：收流与弱网恢复。
#
# 当前有：线格式镜像定义、UDP 套接字、分片重组、接收门面，以及抖动缓冲
# 策略。FEC/NACK 与自适应缓冲接线属于第二阶段。
add_library(sclient_transport STATIC
    src/udp/UdpSocket.cpp
    src/udp/UdpFrameAssembler.cpp
    src/udp/UdpReceiver.cpp
)
add_library(sclient::transport ALIAS sclient_transport)
target_include_directories(sclient_transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sclient_transport
    PUBLIC sclient::base
    PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(sclient_transport PRIVATE ws2_32)
endif()
sclient_set_warnings(sclient_transport)

