#pragma once

#include "core/model/Model.h"

#include <map>
#include <string>

namespace rbank::proto {

/// 跨服协议常量：节点心跳、恢复宽限、汇率轮询节奏。
struct Protocol {
    static constexpr int ratePollSeconds      = 10;
    static constexpr int heartbeatTicks       = 3;
    static constexpr int recoveryGraceSeconds = 60;
    static constexpr int ownerOfflineSeconds  = 300;
};

/// 追加式汇率表即共享协议：各节点轮询当前生效快照，对观察到的变化各公告一次。
class RateBroadcast {
    std::map<std::string, core::Amount> previous;
    bool                                primed = false;

public:
    std::vector<core::Rate> changes(std::vector<core::Rate> const& rates) {
        std::vector<core::Rate> changed;
        for (auto const& rate : rates) {
            auto it = previous.find(rate.server);
            if (primed && (it == previous.end() || it->second != rate.value)) {
                changed.push_back(rate);
            }
            previous[rate.server] = rate.value;
        }
        primed = true;

        return changed;
    }
};

} // namespace rbank::proto
