#pragma once

#include "bootstrap/config/Config.h"
#include "core/port/Ports.h"

namespace rbank::infra {

/// 卡号规则 DSL：字段串按启动配置拼装，唯一性由主键和有限次重试保证。
class CardGenerator final : public core::CardGenerator {
    bootstrap::CardConfig                    cfg;
    std::vector<std::pair<std::string, int>> fields;

public:
    explicit CardGenerator(bootstrap::CardConfig);

    std::string generate(std::string const& xuid, core::Session&) override;
};

} // namespace rbank::infra
