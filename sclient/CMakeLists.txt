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

# 模块按依赖自底向上添加：base → logger → decoding → rendering → transport
#                → mediasec → recording → control → ui → cli → playback → app
# 当前只有 base 与 transport 的纯策略部分（不依赖 Qt / FFmpeg）。
add_subdirectory(modules/base)
add_subdirectory(modules/transport)

if(SCLIENT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()


