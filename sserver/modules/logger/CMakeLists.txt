# logger：结构化日志（logfmt + 控制台/轮转文件双通道）。
#
# spdlog 的引入采用双路径：优先用系统安装的包（开发机通常已装，离线也能编）；
# 找不到再从固定 tag 拉源码编译。为什么不能只 find_package：
# 交叉编译到 RK3576 时，宿主机装了 spdlog 不代表目标 sysroot 里有，
# 那时 FetchContent 跟主工程一起编是唯一可靠的办法。
find_package(spdlog CONFIG QUIET)
if(spdlog_FOUND)
    message(STATUS "logger: 使用系统安装的 spdlog ${spdlog_VERSION}")
else()
    include(FetchContent)
    set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(spdlog
        URL https://github.com/gabime/spdlog/archive/refs/tags/v1.15.3.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP ON)
    FetchContent_MakeAvailable(spdlog)
    message(STATUS "logger: 系统未安装 spdlog，已从源码拉取 v1.15.3")
endif()

add_library(sserver_logger STATIC
    src/Logger.cpp
)
add_library(sserver::logger ALIAS sserver_logger)
target_include_directories(sserver_logger PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# LogMetrics.h 公有头里用了 base 的 LatencySummary，所以 base 是 PUBLIC 依赖；
# spdlog 只出现在实现文件里，PRIVATE 即可，不向使用方泄漏第三方头文件搜索路径。
target_link_libraries(sserver_logger
    PUBLIC sserver::base
    PRIVATE spdlog::spdlog)
sserver_set_warnings(sserver_logger)