# app：客户端入口。
#
# 命令行模式始终可用；--gui 走 GuiRunner，链接 Qt6 时是真窗口，
# 否则由 GuiRunnerStub 给出明确提示。
add_executable(sclient src/main.cpp)
target_include_directories(sclient PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sclient PRIVATE
    sclient::control sclient::transport sclient::decoding sclient::base)

if(SCLIENT_HAS_QT)
    target_sources(sclient PRIVATE src/GuiRunnerQt.cpp)
    target_link_libraries(sclient PRIVATE sclient::rendering Qt6::Widgets)
else()
    target_sources(sclient PRIVATE src/GuiRunnerStub.cpp)
endif()

sclient_set_warnings(sclient)