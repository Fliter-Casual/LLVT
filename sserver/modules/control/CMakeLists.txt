# control：文本控制协议。
#
# 当前只有协议编解码（纯函数）；ControlServer（epoll 反应器 + 命令线程池）
# 在 app 落地时一起加，因为它依赖 StreamingController 的真实实现才能跑起来。
add_library(sserver_control STATIC
    src/ControlProtocol.cpp
    src/ControlServer.cpp
)
add_library(sserver::control ALIAS sserver_control)
target_include_directories(sserver_control PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sserver_control
    PUBLIC sserver::contracts
    PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(sserver_control PRIVATE ws2_32)
endif()
sserver_set_warnings(sserver_control)
