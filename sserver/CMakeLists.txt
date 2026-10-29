cmake_minimum_required(VERSION 3.16) # 最低版本要求

project(sserver
    VERSION 0.1.0
    DESCRIPTION "低延时视频图传系统 - 服务器"
    LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17) # C++17 标准
set(CMAKE_CXX_STANDARD_REQUIRED ON) # 严格要求 C++17 标准
set(CMAKE_CXX_EXTENSIONS OFF) # 不允许使用非标准扩展

# 模块按依赖自底向上添加：base（帧模型/队列/统计） → logger（日志） → reactor（epoll 封装） → appconfig（配置装载） → contracts（控制面协议）
#                → mediasec（媒体加密） → capture（V4L2 采集） → encoding（FFmpeg 编码） → transport（UDP 推流）  → pipeline（共享采集与编码子流） → control（控制协议服务） → app（会话统筹 + main）
# 骨架阶段暂无模块，随后续开发逐个 add_subdirectory。
