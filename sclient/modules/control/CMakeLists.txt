# control：控制协议客户端（连接、命令、响应解析）。
add_library(sclient_control STATIC
    src/ControlResponse.cpp
    src/ControlClient.cpp
)
add_library(sclient::control ALIAS sclient_control)
target_include_directories(sclient_control PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sclient_control PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(sclient_control PRIVATE ws2_32)
endif()
sclient_set_warnings(sclient_control)