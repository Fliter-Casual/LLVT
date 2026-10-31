cmake_minimum_required(VERSION 3.16)

project(sserver
    VERSION 0.1.0
    DESCRIPTION "低延时视频图传系统 - 服务端"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "构建类型" FORCE)
endif()

find_package(Threads REQUIRED)

option(SSERVER_BUILD_TESTS "构建单元测试" ON)
option(SSERVER_WERROR "把编译警告当作错误" OFF)

# 目标平台是 Linux，Windows 上仅用于开发期编译检查，警告集按各编译器能力给。
function(sserver_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(SSERVER_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(SSERVER_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# 模块按依赖自底向上添加：base → logger → reactor → appconfig → contracts
#                → mediasec → capture → encoding → transport → pipeline → control → app
add_subdirectory(modules/base)

if(SSERVER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

