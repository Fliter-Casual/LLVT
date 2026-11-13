# app：会话统筹与程序入口。
#
# ServerApp 实现 contracts::StreamingController，是唯一同时看到"会话表"
# 与"管线"的地方；main 负责参数、配置与停机顺序。
add_library(sserver_app_core STATIC
    src/ServerApp.cpp
)
add_library(sserver::app ALIAS sserver_app_core)
target_include_directories(sserver_app_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sserver_app_core
    PUBLIC sserver::contracts sserver::pipeline sserver::transport sserver::appconfig sserver::base
    PRIVATE Threads::Threads)
sserver_set_warnings(sserver_app_core)

add_executable(sserver src/main.cpp)
target_link_libraries(sserver PRIVATE
    sserver::app sserver::control sserver::logger sserver::appconfig)
sserver_set_warnings(sserver)
