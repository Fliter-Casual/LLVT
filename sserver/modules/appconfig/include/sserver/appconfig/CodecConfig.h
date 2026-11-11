// ============================================================
// 文件：CodecConfig.h
// 模块：sserver::appconfig
// 功能：编码器配置快照：后端选择与码控参数。目前只由代码构造，
//       等 encoding 模块的 TOML 段落地后再从配置文件装载。
// 知识点：默认值即内置配置、码控参数的单位约定
// ============================================================

#ifndef SSERVER_APPCONFIG_CODEC_CONFIG_H
#define SSERVER_APPCONFIG_CODEC_CONFIG_H

#include <cstdint>
#include <string>

namespace sserver {
namespace appconfig {

struct CodecConfig {
    std::string m_backend = "x264";       /**< x264 软编 / mpp 硬编 */
    std::int64_t m_bitrateBps = 800000;   /**< 目标平均码率 */
    int m_gopFrames = 30;                 /**< 固定 GOP：丢包后最坏等一个 GOP 就有 IDR */
    int m_encodeThreads = 1;              /**< 生产默认单线程 + sliced threads，延迟优先 */
    int m_keyframeIntervalMax = 30;       /**< 与 GOP 一致；x264 里必须两个都写 */
    bool m_scenecut = false;              /**< 固定 GOP 必须关闭场景切换检测，否则画面一变就插 IDR */
};

}  // namespace appconfig
}  // namespace sserver

#endif  // SSERVER_APPCONFIG_CODEC_CONFIG_H