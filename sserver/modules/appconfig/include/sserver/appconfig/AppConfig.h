// ============================================================
// 文件：AppConfig.h
// 模块：sserver::appconfig
// 功能：声明服务端配置聚合根 AppConfig 与装载入口 ConfigLoader：从 TOML
//       文件加载、严格校验（未知键、类型、范围），错误信息带源位置。
// 知识点：聚合根模式、默认值即内置配置、TOML 解析、错误定位信息回传
// ============================================================

#ifndef SSERVER_APPCONFIG_APP_CONFIG_H
#define SSERVER_APPCONFIG_APP_CONFIG_H

#include <cstddef>
#include <string>
#include "sserver/appconfig/CodecConfig.h"

namespace sserver {
namespace appconfig {

// ==================== 各段配置 ====================
//
// 当前只覆盖四个字段，目的是先立起「严格校验 + 行号定位」这套机制。
// 后续 capture/encoding/transport 落地时按需往结构体里加成员、往加载器
// 里加一行映射即可，校验逻辑不用重写。

struct RuntimeConfig {
    bool m_logVerbose = false; /**< [runtime] log_verbose 默认info, true->Debug*/
};

struct ControlConfig {
    std::string m_bindAddress = "0.0.0.0"; /**< [control] bind_address */
    int m_port = 9000;              /**< [control] port，控制通道 TCP 监听端口 */
    std::size_t m_maxClients = 16;  /**< [control] max_clients，并发控制连接上限 */
};

struct CaptureConfig {
    std::string m_device = "auto"; /**< [capture] device，"auto" 表示启动时扫描 /dev/video* */
};

// ==================== 配置聚合根 ====================

// 成员默认值就是内置默认配置：没有配置文件或文件里省略某个键时，
// 用的就是这里的值，不需要额外维护一份"默认配置表"。
struct AppConfig {
    RuntimeConfig m_runtime;
    ControlConfig m_control;
    CaptureConfig m_capture;
    CodecConfig m_codec;

    static AppConfig createDefault();

    /**
     * 校验整份配置（字段范围与组合约束）。
     *
     * @param errorMessage 非空时输出首个错误，形如
     *        "control.port must be in [1, 65535]"
     * @param errorField 非空时输出出错的 "表.键"，无法定位时为空
     * @return 全部通过返回 true
     */
    bool validate(std::string *errorMessage, std::string *errorField = nullptr) const;
};

// ==================== 配置装载入口 ====================

// 纯静态工具类：装载流程无实例状态，调用方从不创建对象。
//
// 失败时 config 保持原值不变——调用方可以放心地在"加载失败即退出"和
// "加载失败沿用默认"之间选择，不会被半加载的配置坑到。
class ConfigLoader {
public:
    /**
     * 从 TOML 文件加载：内置默认值打底 → 应用文件内容 → 整体校验。
     *
     * @param errorMessage 非空时输出 "文件:行:列: 描述"
     */
    static bool loadFromFile(const std::string &filePath, AppConfig *config,
                             std::string *errorMessage);

    // 直接从文本加载，错误信息里的文件名标为 sourceName。
    // 供单测免去写临时文件；生产路径走 loadFromFile。
    static bool loadFromString(const std::string &tomlText, const std::string &sourceName,
                               AppConfig *config, std::string *errorMessage);
};

}  // namespace appconfig
}  // namespace sserver

#endif  // SSERVER_APPCONFIG_APP_CONFIG_H
