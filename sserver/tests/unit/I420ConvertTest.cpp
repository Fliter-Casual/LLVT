// I420Convert：YUYV422 → I420 转换的单元测试。
//
// YUYV422 每两个像素占 4 字节：Y0 U Y1 V。所以每个用例的源缓冲都能手算：
// 亮度取每 4 字节里的第 0、2 个，色度取第 1、3 个，再对上下两行取平均。
#include <cstddef>
#include <cstdint>
#include <vector>

#include "sserver/encoding/I420Convert.h"
#include "support/Check.h"

namespace {

using sserver::encoding::convertYuyv422ToI420;
using sserver::encoding::I420Planes;

// 一组平面缓冲，连同各自的输出行距。
struct Destinations {
    std::vector<std::uint8_t> m_y;
    std::vector<std::uint8_t> m_u;
    std::vector<std::uint8_t> m_v;
    I420Planes planes(std::size_t yStride, std::size_t uStride, std::size_t vStride) {
        I420Planes result;
        result.m_y = m_y.data();
        result.m_u = m_u.data();
        result.m_v = m_v.data();
        result.m_yStride = yStride;
        result.m_uStride = uStride;
        result.m_vStride = vStride;
        return result;
    }
};

Destinations makeDestinations(int height,
                              std::size_t yStride, std::size_t uStride, std::size_t vStride) {
    Destinations destinations;
    destinations.m_y.assign(yStride * static_cast<std::size_t>(height), 0);
    destinations.m_u.assign(uStride * static_cast<std::size_t>(height / 2), 0);
    destinations.m_v.assign(vStride * static_cast<std::size_t>(height / 2), 0);
    return destinations;
}

void testBasicConversion() {
    // 4x2：每行两个像素对，上下两行的色度不同以验证垂直平均。
    const std::vector<std::uint8_t> source = {
            10, 100, 11, 110, 12, 120, 13, 130,   // 第 0 行
            20, 100, 21, 110, 22, 120, 23, 130,   // 第 1 行
    };
    auto destinations = makeDestinations(2, 4, 2, 2);
    const I420Planes planes = destinations.planes(4, 2, 2);

    CHECK(convertYuyv422ToI420(source.data(), 8, planes, 4, 2));

    // 亮度：逐行按 4 字节步进抽取第 0、2 个。
    CHECK(destinations.m_y[0] == 10);
    CHECK(destinations.m_y[1] == 11);
    CHECK(destinations.m_y[2] == 12);
    CHECK(destinations.m_y[3] == 13);
    CHECK(destinations.m_y[4] == 20);
    CHECK(destinations.m_y[5] == 21);
    CHECK(destinations.m_y[6] == 22);
    CHECK(destinations.m_y[7] == 23);

    // 色度：两行相同，平均后仍是原值。
    CHECK(destinations.m_u[0] == 100);
    CHECK(destinations.m_u[1] == 120);
    CHECK(destinations.m_v[0] == 110);
    CHECK(destinations.m_v[1] == 130);
}

void testChromaAveragesVertically() {
    // 4x4：四行的色度各不相同，验证两个色度行分别对应两组行对。
    std::vector<std::uint8_t> source;
    const int kChromaU[4] = {100, 200, 50, 150};
    const int kChromaV[4] = {110, 210, 60, 160};
    for (int row = 0; row < 4; ++row) {
        for (int pair = 0; pair < 2; ++pair) {
            source.push_back(0);                            // Y0
            source.push_back(static_cast<std::uint8_t>(kChromaU[row]));
            source.push_back(0);                            // Y1
            source.push_back(static_cast<std::uint8_t>(kChromaV[row]));
        }
    }
    auto destinations = makeDestinations(4, 4, 2, 2);
    const I420Planes planes = destinations.planes(4, 2, 2);

    CHECK(convertYuyv422ToI420(source.data(), 8, planes, 4, 4));

    // 第 0 色度行 = 行 0/1 平均：(100+200+1)/2 = 150，(110+210+1)/2 = 160
    CHECK(destinations.m_u[0] == 150);
    CHECK(destinations.m_u[1] == 150);
    CHECK(destinations.m_v[0] == 160);
    CHECK(destinations.m_v[1] == 160);

    // 第 1 色度行 = 行 2/3 平均：(50+150+1)/2 = 100，(60+160+1)/2 = 110
    CHECK(destinations.m_u[2] == 100);
    CHECK(destinations.m_u[3] == 100);
    CHECK(destinations.m_v[2] == 110);
    CHECK(destinations.m_v[3] == 110);
}

void testSourceStridePaddingIsSkipped() {
    // 源缓冲行尾有 2 字节填充：填充值绝不能出现在输出里。
    const std::vector<std::uint8_t> source = {
            1, 2, 3, 4, 99, 99,
            5, 6, 7, 8, 99, 99,
    };
    auto destinations = makeDestinations(2, 2, 1, 1);
    const I420Planes planes = destinations.planes(2, 1, 1);

    CHECK(convertYuyv422ToI420(source.data(), 6, planes, 2, 2));

    CHECK(destinations.m_y[0] == 1);
    CHECK(destinations.m_y[1] == 3);
    CHECK(destinations.m_y[2] == 5);
    CHECK(destinations.m_y[3] == 7);
    CHECK(destinations.m_u[0] == 4);   // (2+6+1)/2
    CHECK(destinations.m_v[0] == 6);   // (4+8+1)/2
}

void testDestinationStridePaddingIsUntouched() {
    // 输出行距大于宽度时，行尾那几字节不属于本函数，不能被写脏。
    const std::vector<std::uint8_t> source = {
            1, 2, 3, 4,
            5, 6, 7, 8,
    };
    Destinations destinations;
    const std::size_t kYStride = 4;   // 宽 2，行尾留 2 字节
    const std::size_t kUvStride = 2;  // 色度宽 1，行尾留 1 字节
    destinations.m_y.assign(kYStride * 2, 0xEE);
    destinations.m_u.assign(kUvStride * 1, 0xEE);
    destinations.m_v.assign(kUvStride * 1, 0xEE);

    const I420Planes planes = destinations.planes(kYStride, kUvStride, kUvStride);
    CHECK(convertYuyv422ToI420(source.data(), 4, planes, 2, 2));

    CHECK(destinations.m_y[0] == 1 && destinations.m_y[1] == 3);
    CHECK(destinations.m_y[2] == 0xEE && destinations.m_y[3] == 0xEE);
    CHECK(destinations.m_y[4] == 5 && destinations.m_y[5] == 7);
    CHECK(destinations.m_y[6] == 0xEE && destinations.m_y[7] == 0xEE);
    CHECK(destinations.m_u[0] == 4);
    CHECK(destinations.m_u[1] == 0xEE);
    CHECK(destinations.m_v[0] == 6);
    CHECK(destinations.m_v[1] == 0xEE);
}

void testRejectsInvalidParameters() {
    const std::vector<std::uint8_t> source(64, 0);

    // 宽或高为奇数：4:2:0 无法均分色度平面。
    {
        auto destinations = makeDestinations(2, 4, 2, 2);
        const I420Planes planes = destinations.planes(4, 2, 2);
        CHECK(!convertYuyv422ToI420(source.data(), 8, planes, 3, 2));
        CHECK(!convertYuyv422ToI420(source.data(), 8, planes, 4, 1));
    }
    // 非正尺寸。
    {
        auto destinations = makeDestinations(2, 4, 2, 2);
        const I420Planes planes = destinations.planes(4, 2, 2);
        CHECK(!convertYuyv422ToI420(source.data(), 8, planes, 0, 2));
        CHECK(!convertYuyv422ToI420(source.data(), 8, planes, -2, 2));
    }
    // 空指针。
    {
        auto destinations = makeDestinations(2, 4, 2, 2);
        I420Planes planes = destinations.planes(4, 2, 2);
        CHECK(!convertYuyv422ToI420(nullptr, 8, planes, 4, 2));
        planes.m_u = nullptr;
        CHECK(!convertYuyv422ToI420(source.data(), 8, planes, 4, 2));
    }
    // 行距不足：源行距小于宽×2，或输出行距小于对应平面宽度。
    {
        auto destinations = makeDestinations(2, 4, 2, 2);
        I420Planes planes = destinations.planes(4, 2, 2);
        CHECK(!convertYuyv422ToI420(source.data(), 7, planes, 4, 2));
        planes = destinations.planes(3, 2, 2);
        CHECK(!convertYuyv422ToI420(source.data(), 8, planes, 4, 2));
        planes = destinations.planes(4, 1, 2);
        CHECK(!convertYuyv422ToI420(source.data(), 8, planes, 4, 2));
        planes = destinations.planes(4, 2, 0);
        CHECK(!convertYuyv422ToI420(source.data(), 8, planes, 4, 2));
    }
}

}  // namespace

int main() {
    testBasicConversion();
    testChromaAveragesVertically();
    testSourceStridePaddingIsSkipped();
    testDestinationStridePaddingIsUntouched();
    testRejectsInvalidParameters();
    return sserver::test::finish("i420_convert_test");
}
