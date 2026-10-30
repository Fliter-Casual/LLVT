# tests：单元测试。测试只链接模块 target，不直接引用模块源码文件。
#
# 每个用例是可执行文件 + CTest 用例；断言用 support/Check.h，不引入外部框架。

# ==============================================================================
# 自定义函数：add_server_unit_test
# 作用：一键注册并配置一个单元测试用例
# 参数：
#   <source>   - 测试源文件名（位于当前目录的 unit/ 文件夹下，如 VideoCodecTest.cpp）
#   <target>   - 生成的可执行文件名（如 video_codec_test）
#   <label>    - 给 CTest 打的标签（分号分隔，如 "unit;base"），方便筛选测试
#   [依赖模块...] - 变长参数（ARGN），表示该测试需要链接的库目标（如 sserver::base）
# ==============================================================================
function(add_server_unit_test source target label)
    # 1. 创建测试可执行文件（源文件路径自动拼接为 unit/ 目录）
    add_executable(${target} unit/${source})

    # 2. 链接依赖库。${ARGN} 接收了函数调用时传入的所有后续模块（如 sserver::base）
    #    使用 PRIVATE，表示这些依赖仅供该测试程序使用，不向外传递。
    target_link_libraries(${target} PRIVATE ${ARGN})

    # 3. 添加头文件搜索路径。设为当前测试目录（方便测试代码 #include "support/Check.h" 等）
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    # 4. 应用项目全局定义的编译警告策略（如 -Wall -Wextra），确保测试代码也保持高质量
    sserver_set_warnings(${target})

    # 5. 向 CTest 注册这个测试用例。CTest 运行时，会执行这个可执行文件并根据返回值判断成败
    add_test(NAME ${target} COMMAND ${target})

    # 6. 设置测试属性：
    #    LABELS  - 打上标签（如 "unit;base"），方便在跑 CTest 时用 -L 参数只跑特定标签的测试
    #    TIMEOUT - 设置超时时间为 30 秒（防止死锁或死循环导致 CI 卡死）
    set_tests_properties(${target} PROPERTIES LABELS "${label}" TIMEOUT 30)
endfunction()


# ==============================================================================
# 批量注册具体的单元测试用例
# ==============================================================================

# 测试 VideoCodec 模块（标签：单元测试、基础模块）
add_server_unit_test(VideoCodecTest.cpp video_codec_test "unit;base" sserver::base)

# 测试线程安全队列（标签：单元测试、基础模块、并发），重点测试并发逻辑
add_server_unit_test(ThreadSafeQueueTest.cpp thread_safe_queue_test "unit;base;concurrency" sserver::base)

# 测试基础帧模型（标签：单元测试、基础模块）
add_server_unit_test(BaseFrameModelTest.cpp base_frame_model_test "unit;base" sserver::base)
