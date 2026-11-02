# tests：单元测试。测试只链接模块 target，不直接引用模块源码文件。

function(add_client_unit_test source target label)
    add_executable(${target} unit/${source})
    target_link_libraries(${target} PRIVATE ${ARGN})
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    sclient_set_warnings(${target})
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES LABELS "${label}" TIMEOUT 30)
endfunction()

add_client_unit_test(LatencyStatsTest.cpp latency_stats_test "unit;base;stats" sclient::base)
add_client_unit_test(AdaptiveJitterBufferTest.cpp adaptive_jitter_buffer_test
        "unit;transport;jitter" sclient::transport)
