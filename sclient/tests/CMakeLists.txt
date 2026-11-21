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
add_client_unit_test(StreamProtocolMirrorTest.cpp stream_protocol_mirror_test
        "unit;transport;protocol" sclient::transport)
add_client_unit_test(UdpFrameAssemblerTest.cpp udp_frame_assembler_test
        "unit;transport;assembly" sclient::transport)
add_client_unit_test(UdpReceiverTest.cpp udp_receiver_test
        "integration;transport;udp" sclient::transport)
set_tests_properties(udp_receiver_test PROPERTIES TIMEOUT 60)
add_client_unit_test(ControlResponseTest.cpp control_response_test
        "unit;control;protocol" sclient::control)
add_client_unit_test(ControlClientTest.cpp control_client_test
        "integration;control;tcp" sclient::control)
set_tests_properties(control_client_test PROPERTIES TIMEOUT 60)
add_client_unit_test(VideoDecoderTest.cpp video_decoder_test
        "unit;decoding" sclient::decoding)
