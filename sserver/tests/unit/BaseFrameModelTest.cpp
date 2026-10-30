// 帧模型：RawVideoFrame 的租约与视图语义、EncodedFrame 默认值的单元测试。
//
// 这里验证的是全链路最容易出错的一点：V4L2 缓冲的归还必须恰好发生一次，
// 且发生在最后一个持有者释放时——队列丢帧、管线重启、异常退出都依赖它。
#include <cstdint>
#include <memory>
#include <vector>

#include "sserver/base/EncodedFrame.h"
#include "sserver/base/RawVideoFrame.h"
#include "support/Check.h"

namespace {

using sserver::base::EncodedFrame;
using sserver::base::RawVideoFrame;
using sserver::base::RawVideoFramePtr;
using sserver::base::RawVideoPixelFormat;
using sserver::base::StreamPayloadType;
using sserver::base::codec::VideoCodec;

void testOwnedFrameView() {
    RawVideoFrame frame;
    frame.m_data = std::vector<std::uint8_t>{1, 2, 3, 4};
    frame.m_bytesUsed = frame.m_data.size();
    frame.m_pixelFormat = RawVideoPixelFormat::Yuyv422;
    frame.m_width = 2;
    frame.m_height = 1;
    frame.m_stride = 4;

    const auto view = frame.view();
    CHECK(view.m_pixelFormat == RawVideoPixelFormat::Yuyv422);
    CHECK(view.m_data == frame.m_data.data());
    CHECK(view.m_bytesUsed == 4);
    CHECK(view.m_width == 2);
    CHECK(view.m_height == 1);
    CHECK(view.m_stride == 4);
    CHECK(view.m_dmaBufFd == -1);
}

void testEmptyFrameView() {
    RawVideoFrame frame;
    const auto view = frame.view();
    CHECK(view.m_data == nullptr);
    CHECK(view.m_pixelFormat == RawVideoPixelFormat::Unknown);
    CHECK(view.m_bytesUsed == 0);
}

void testExternalDataTakesPrecedence() {
    // 零拷贝路径：视图应指向 mmap 缓冲而不是自有向量。
    const std::uint8_t external[8] = {9, 9, 9, 9, 9, 9, 9, 9};
    RawVideoFrame frame;
    frame.m_data = std::vector<std::uint8_t>{1, 2};
    frame.m_externalData = external;
    frame.m_bytesUsed = 8;
    frame.m_stride = 8;

    const auto view = frame.view();
    CHECK(view.m_data == external);
    CHECK(view.m_bytesUsed == 8);
}

void testReleaseCallbackFiresOnceOnDestroy() {
    int releaseCount = 0;
    {
        RawVideoFrame frame;
        frame.m_releaseCallback = [&releaseCount]() { ++releaseCount; };
        CHECK(releaseCount == 0);
    }
    CHECK(releaseCount == 1);
}

void testReleaseCallbackFiresOnLastSharedOwner() {
    int releaseCount = 0;
    {
        auto frame = std::make_shared<RawVideoFrame>();
        frame->m_releaseCallback = [&releaseCount]() { ++releaseCount; };

        RawVideoFramePtr first = frame;
        RawVideoFramePtr second = frame;
        frame.reset();
        first.reset();
        CHECK(releaseCount == 0);  // 还有一个持有者

        second.reset();
        CHECK(releaseCount == 1);  // 最后一个释放时才归还
    }
    CHECK(releaseCount == 1);
}

void testMoveTransfersLease() {
    int releaseCount = 0;

    RawVideoFrame source;
    source.m_releaseCallback = [&releaseCount]() { ++releaseCount; };
    source.m_bytesUsed = 5;
    source.m_width = 160;

    RawVideoFrame target(std::move(source));
    CHECK(target.m_releaseCallback != nullptr);
    CHECK(target.m_bytesUsed == 5);
    CHECK(target.m_width == 160);
    CHECK(releaseCount == 0);  // 移动本身不归还

    // 被移走的一方不再持有租约，析构它不应触发回调。
    // source 在作用域结束时析构，target 也一样，总计只能发生一次。
    {
        RawVideoFrame moved;
        moved = std::move(target);
        CHECK(moved.m_releaseCallback != nullptr);
        CHECK(target.m_releaseCallback == nullptr);
    }
    CHECK(releaseCount == 1);
}

void testMoveAssignReleasesPreviousLease() {
    int firstReleased = 0;
    int secondReleased = 0;

    RawVideoFrame first;
    first.m_releaseCallback = [&firstReleased]() { ++firstReleased; };

    RawVideoFrame second;
    second.m_releaseCallback = [&secondReleased]() { ++secondReleased; };

    // 目标原本持有自己的租约，被覆盖时必须先归还，否则缓冲泄漏。
    first = std::move(second);
    CHECK(firstReleased == 1);
    CHECK(secondReleased == 0);

    first = RawVideoFrame();  // 再次覆盖：归还第二个租约
    CHECK(secondReleased == 1);
}

void testEncodedFrameDefaults() {
    EncodedFrame frame;
    CHECK(frame.m_type == StreamPayloadType::Video);
    CHECK(frame.m_sequence == 0);
    CHECK(frame.m_encodingId == 0);
    CHECK(!frame.m_isKeyframe);
    CHECK(frame.m_videoCodec == VideoCodec::kH264);
    CHECK(frame.m_payload.empty());
    // 线上 subType 的数值是两端共享的常量，改动会破坏兼容。
    CHECK(static_cast<std::uint16_t>(StreamPayloadType::Video) == 3);
}

}  // namespace

int main() {
    testOwnedFrameView();
    testEmptyFrameView();
    testExternalDataTakesPrecedence();
    testReleaseCallbackFiresOnceOnDestroy();
    testReleaseCallbackFiresOnLastSharedOwner();
    testMoveTransfersLease();
    testMoveAssignReleasesPreviousLease();
    testEncodedFrameDefaults();
    return sserver::test::finish("base_frame_model_test");
}
