# appconfig：TOML 配置装载与严格校验。
#
# 当前只覆盖四个字段（control.port / control.max_clients / capture.device /
# runtime.log_verbose），先把「未知键、类型、范围 + 源位置定位」这套机制立起来。
# 后续加字段只需往结构体加成员、往加载器加一行映射。
# 优先找系统库
find_package(tomlplusplus CONFIG QUIET)
if(tomlplusplus_FOUND)
    message(STATUS "appconfig: 使用系统安装的 toml++ ${tomlplusplus_VERSION}")
else()
# 如果系统没装，才走 FetchContent 下载（你原来的代码）
    include(FetchContent)
    FetchContent_Declare(tomlplusplus
        URL https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP ON)
    FetchContent_MakeAvailable(tomlplusplus)
    message(STATUS "appconfig: 系统未安装 toml++，已从源码拉取 v3.4.0")
endif()

add_library(sserver_appconfig STATIC
    src/AppConfig.cpp
)
add_library(sserver::appconfig ALIAS sserver_appconfig)
target_include_directories(sserver_appconfig PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# toml++ 只出现在实现文件里，用 PRIVATE 不把第三方头文件搜索路径泄漏给上层。
target_link_libraries(sserver_appconfig PRIVATE tomlplusplus::tomlplusplus)
sserver_set_warnings(sserver_appconfig)
