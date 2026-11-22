# rendering：Qt6 + OpenGL 显示。
#
# Qt6 是可选依赖：找到就构建窗口版客户端，找不到只保留命令行版。
if(NOT SCLIENT_HAS_QT)
    message(STATUS "rendering: 未找到 Qt6 —— 不构建图形界面")
    return()
endif()
add_library(sclient_rendering STATIC
    src/Renderer.cpp
    src/VideoWidget.cpp
)
add_library(sclient::rendering ALIAS sclient_rendering)
target_include_directories(sclient_rendering PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sclient_rendering
    PUBLIC sclient::decoding
    PRIVATE Qt6::Widgets Qt6::OpenGLWidgets Qt6::OpenGL)
sclient_set_warnings(sclient_rendering)