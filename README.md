# LLVT
Low-Latency Video Transmission system for real-time audio/video streaming.
# 低延时视频图传系统

面向嵌入式设备远程监控的低延时视频图传系统，C++17 实现，分两个程序：

- **sserver**：服务端，常驻运行在 Linux（RK3576 开发板或任意 Linux 机器），
  V4L2 采集摄像头画面，编码为 H.264/H.265，经 UDP 推流。
- **sclient**：客户端，Qt6 跨平台程序（Linux / Windows / macOS），
  连接服务端、收流、FFmpeg 解码、OpenGL 渲染。

两个程序各自是独立的 CMake 工程，并排放在本仓库下；
模块结构、日志格式、二进制线格式保持对应，联调时两边能一一对应。

## 架构总览

```
V4L2 摄像头 --> sserver (capture → encoding → transport)
                    ↑ 控制通道 :9000/TCP（STATUS · CAPS · START · STOP · KEYFRAME）
                    ↓ 媒体流 UDP（分片 + FEC + NACK）
               sclient (transport → playback → FFmpeg 解码 → OpenGL 渲染)
```

详细设计见 [docs/服务端模块说明.md](docs/服务端模块说明.md) 与
[docs/客户端模块说明.md](docs/客户端模块说明.md)。

## v0.1 目标（第一阶段里程碑）

最小可玩路径：服务端 MMAP 采集 + x264 软编 + UDP 分片推流；
客户端收流组装 + FFmpeg 软解 + Qt/OpenGL 显示。
含断档检测与 KEYFRAME 请求、令牌桶发送整形、会话租约超时回收。

第二阶段（backlog）：分组 FEC、NACK 重传与熔断、自适应 jitter buffer、
自动画质切档、多客户端共享采集/编码、MP4 录像、RK 硬件链路
（DMA-BUF / RGA / MPP）、AES-GCM 媒体加密、统计面板。

## 构建（占位，随工程填充）

```bash
# sserver（Linux）
cmake -S sserver -B sserver/build
cmake --build sserver/build

# sclient（需 Qt6 与 FFmpeg）
cmake -S sclient -B sclient/build
cmake --build sclient/build
```