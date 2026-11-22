cmake_minimum_required(VERSION 3.16)

project(sclient
    VERSION 0.1.0
    DESCRIPTION "低延时视频图传系统 - 客户端"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "构建类型" FORCE)
endif()

find_package(Threads REQUIRED)

option(SCLIENT_BUILD_TESTS "构建单元测试" ON)
option(SCLIENT_WERROR "把编译警告当作错误" OFF)

function(sclient_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(SCLIENT_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(SCLIENT_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# Qt6 是可选依赖：找到就构建窗口版客户端，找不到只保留命令行版。
find_package(Qt6 QUIET COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets)
if(Qt6_FOUND)
    set(SCLIENT_HAS_QT TRUE)
    message(STATUS "sclient: 找到 Qt6 ${Qt6_VERSION}，将构建图形界面")
else()
    set(SCLIENT_HAS_QT FALSE)
    message(STATUS "sclient: 未找到 Qt6，只构建命令行客户端")
endif()

# 模块按依赖自底向上添加：base → logger → decoding → rendering → transport
#                → mediasec → recording → control → ui → cli → playback → app

add_subdirectory(modules/base)
add_subdirectory(modules/transport)
add_subdirectory(modules/decoding)
add_subdirectory(modules/rendering)
add_subdirectory(modules/control)
add_subdirectory(modules/app)

if(SCLIENT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()


