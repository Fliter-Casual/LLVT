// FormatSelectionPolicy：候选尺寸过滤、排序与步进对齐的单元测试。
//
// 这部分逻辑纯粹是决策，不碰 ioctl，所以摄像头不在也能验证"驱动返回这组
// 尺寸时，我们会按什么顺序去试"。
#include <cstdint>
#include <vector>

#include "sserver/capture/FormatSelectionPolicy.h"
#include "support/Check.h"

namespace {

using sserver::capture::alignToStep;
using sserver::capture::CaptureSize;
using sserver::capture::fitsCaptureRequest;
using sserver::capture::preferredCaptureSizes;
using sserver::capture::rankCaptureSizes;

bool same(const CaptureSize &left, const CaptureSize &right) {
    return left.m_width == right.m_width && left.m_height == right.m_height;
}

void testFitsCaptureRequest() {
    // 正常范围内、偶数尺寸可用。
    CHECK(fitsCaptureRequest({640, 480}, 1920, 1080));
    CHECK(fitsCaptureRequest({1920, 1080}, 1920, 1080));

    // 超过请求上限不可用：编码器按上限配置，超出要么缩放要么溢出。
    CHECK(!fitsCaptureRequest({1920, 1080}, 1280, 720));
    CHECK(!fitsCaptureRequest({1920, 1082}, 1920, 1080));

    // 奇数尺寸不可用：4:2:0 的色度平面无法均分。
    CHECK(!fitsCaptureRequest({641, 480}, 1920, 1080));
    CHECK(!fitsCaptureRequest({640, 481}, 1920, 1080));

    // 太小或为零。
    CHECK(!fitsCaptureRequest({0, 0}, 1920, 1080));
    CHECK(!fitsCaptureRequest({1, 1}, 1920, 1080));
    CHECK(fitsCaptureRequest({2, 2}, 1920, 1080));
}

void testAlignToStep() {
    // 正好落在格点上。
    CHECK(alignToStep(1920, 32, 1920, 32) == 1920);
    // 向下对齐：32 + floor((1000-32)/320)*320 = 32 + 3*320 = 992。
    CHECK(alignToStep(1000, 32, 1920, 320) == 992);
    // 期望值超过上限时先夹到上限再对齐。
    CHECK(alignToStep(4000, 32, 1920, 32) == 1920);
    // 步进为 1（连续范围）：原样返回，但奇数要退回一步以保证偶数。
    CHECK(alignToStep(640, 1, 1920, 1) == 640);
    CHECK(alignToStep(641, 1, 1920, 1) == 640);

    // 非法输入一律返回 0，调用方据此丢弃该候选。
    CHECK(alignToStep(1000, 1920, 32, 32) == 0);   // 下限大于上限
    CHECK(alignToStep(100, 200, 1920, 32) == 0);   // 期望低于下限
    CHECK(alignToStep(1000, 32, 1920, 0) == 0);    // 步进为零
    // 步进为奇数且退一步会掉到下限以下：无解。
    CHECK(alignToStep(1, 1, 200, 2) == 0);
}

void testPreferredSizesStartWithRequest() {
    const std::vector<CaptureSize> sizes = preferredCaptureSizes(1920, 1080);
    CHECK(!sizes.empty());
    CHECK(same(sizes.front(), {1920, 1080}));
    // 常见退路档位必须在清单里，否则驱动不支持请求尺寸时会直接失败。
    bool has720p = false;
    bool hasVga = false;
    for (const CaptureSize &size : sizes) {
        if (same(size, {1280, 720})) {
            has720p = true;
        }
        if (same(size, {640, 480})) {
            hasVga = true;
        }
    }
    CHECK(has720p);
    CHECK(hasVga);
}

void testRankingPutsExactRequestFirst() {
    const std::vector<CaptureSize> available = {
            {1920, 1080}, {1280, 720}, {800, 600}, {640, 480},
    };
    // 请求 1080p：精确匹配打头，720p 次之，其余按面积降序。
    const std::vector<CaptureSize> ranked = rankCaptureSizes(available, 1920, 1080);
    CHECK(ranked.size() == 4);
    CHECK(same(ranked[0], {1920, 1080}));
    CHECK(same(ranked[1], {1280, 720}));
    CHECK(same(ranked[2], {800, 600}));
    CHECK(same(ranked[3], {640, 480}));
}

void testRankingPrefers720pOverLargerArea() {
    // 请求尺寸同时是上限，所以这里能进入排序的候选都 ≤ 1080p。
    // 1600x1000 的面积（160 万）比 1280x720（92 万）大得多，但 720p 是
    // 专门列出的"高清但软编跑得动"的分界档，优先级必须高于面积。
    const std::vector<CaptureSize> available = {
            {1600, 1000}, {1280, 720},
    };
    const std::vector<CaptureSize> ranked = rankCaptureSizes(available, 1920, 1080);
    CHECK(ranked.size() == 2);
    CHECK(same(ranked[0], {1280, 720}));
    CHECK(same(ranked[1], {1600, 1000}));
}

void testRankingRespectsRequestAsUpperBound() {
    // 请求 640x480 时，1080p 与 720p 都超过上限，必须被过滤掉——
    // 编码器是按请求上限配置的，收下超尺寸只会带来缩放或溢出。
    const std::vector<CaptureSize> available = {
            {1920, 1080}, {1280, 720}, {800, 600}, {640, 480},
    };
    const std::vector<CaptureSize> ranked = rankCaptureSizes(available, 640, 480);
    CHECK(ranked.size() == 1);
    CHECK(same(ranked[0], {640, 480}));
}

void testRankingFiltersAndDeduplicates() {
    const std::vector<CaptureSize> available = {
            {1920, 1080},
            {1920, 1080},   // 重复
            {641, 481},     // 奇数，过滤
            {0, 0},         // 非法，过滤
            {2560, 1440},   // 超过请求上限，过滤
            {640, 480},
    };
    const std::vector<CaptureSize> ranked = rankCaptureSizes(available, 1920, 1080);
    CHECK(ranked.size() == 2);
    CHECK(same(ranked[0], {1920, 1080}));
    CHECK(same(ranked[1], {640, 480}));
}

void testRankingEmptyCandidates() {
    CHECK(rankCaptureSizes({}, 1920, 1080).empty());
    // 全部越界时结果为空，调用方据此判定"驱动没有可用模式"。
    CHECK(rankCaptureSizes({{4000, 3000}}, 1920, 1080).empty());
}

}  // namespace

int main() {
    testFitsCaptureRequest();
    testAlignToStep();
    testPreferredSizesStartWithRequest();
    testRankingPutsExactRequestFirst();
    testRankingPrefers720pOverLargerArea();
    testRankingRespectsRequestAsUpperBound();
    testRankingFiltersAndDeduplicates();
    testRankingEmptyCandidates();
    return sserver::test::finish("format_selection_policy_test");
}
