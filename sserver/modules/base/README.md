# base 模块：帧模型与通用工具

零依赖的仓库最底层，回答两个基础问题：一帧数据长什么样，线程之间怎么把帧交来交去。
不含任何业务逻辑，被上层全部模块共用。

## 内容

| 文件 | 作用 |
|---|---|
| `VideoCodec.h` | H.264/H.265 编码种类、线格式取值、NAL 类型提取、随机接入点与参数集判定 |
| `RawVideoFrame.h` | 原始帧（自有字节 / mmap 零拷贝引用 / DMA-BUF 句柄）+ 只读视图 + RAII 归还钩子 |
| `EncodedFrame.h` | 编码帧：序号、子流标识、采集/编码时间戳、关键帧标志、Annex B 载荷 |
| `ThreadSafeQueue.h` | 有界线程安全队列：满丢最旧、按谓词选择性丢弃、限时阻塞弹出 |

## 设计要点

- **帧的两种所有权**：`RawVideoFrame` 要么持有拷贝，要么挂一个 V4L2 缓冲的租约。
  租约由 `m_releaseCallback` 在析构时归还，帧经 `shared_ptr` 流转，最后一个持有者
  释放即归还，不需要任何人记得「用完还回去」。
- **视图不持有数据**：`RawVideoFrameView` 只是指针加几何信息，不延长帧寿命；`m_stride`
  携带行跨距，行尾填充必须被下游感知。
- **队列不做关闭语义**：容量预算每次入队由调用方传入；关停靠 `notifyAll()` 加外部
  停止标志，所以队列上的等待一律有超时。
- **丢帧要计数**：`pushDropOldestCountDropped` / `pushDropSelective` 返回丢弃数，
  丢帧统计是有意义的观测数据。

## 约定

- 时间戳统一用单调时钟纳秒，与墙钟无关。
- 编码帧入队后内容不可变，以 `EncodedFramePtr`（`shared_ptr<const T>`）共享。