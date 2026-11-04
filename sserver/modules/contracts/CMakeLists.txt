# contracts：控制面契约（纯头文件接口库）。
add_library(sserver_contracts INTERFACE)
add_library(sserver::contracts ALIAS sserver_contracts)
target_include_directories(sserver_contracts INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)