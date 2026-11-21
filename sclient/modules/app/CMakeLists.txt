# app：客户端入口。当前是命令行模式：连接 → START → 收帧 → 续租 → STOP。
# Qt 图形界面与播放编排（playback/decoding/rendering/ui）随后接入。
add_executable(sclient src/main.cpp)
target_link_libraries(sclient PRIVATE
    sclient::control sclient::transport sclient::decoding sclient::base)
sclient_set_warnings(sclient)