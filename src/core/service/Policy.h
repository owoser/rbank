#pragma once

#include "core/port/Ports.h"

#include <algorithm>

namespace rbank::core {

/// 手续费一律在 base 层计收：ceil(base × ppm / 1e6) + fixed，向上取整。
struct FeePolicy {
    static Amount calculate(Amount base, Fee fee) {
        require(base >= 0 && fee.ppm >= 0 && fee.ppm <= 1'000'000 && fee.fixed >= 0, "CONFIG", "手续费配置无效");

        // 拆分乘法，使 INT64_MAX 也能计价而不溢出。
        auto whole    = times(base / 1'000'000, fee.ppm);
        auto rest     = (base % 1'000'000) * fee.ppm;
        auto rounding = rest % 1'000'000 != 0 ? 1 : 0;

        return plus(plus(whole, rest / 1'000'000 + rounding), fee.fixed);
    }

    /// 本服货币折算为 base；非整倍一律拒绝，不做静默截断。
    static Amount convert(Amount local, Amount rate) {
        require(rate > 0, "RATE", "本服汇率未配置");
        require(local > 0, "AMOUNT", "金额必须为正整数");
        require(local % rate == 0, "MULTIPLE", "本服最小可操作金额为 " + std::to_string(rate) + "，请输入其整数倍");

        return local / rate;
    }
};

struct SlotService {
    static int available(std::vector<Card> const& cards, int limit) {
        require(limit > 0 && limit <= 100, "CONFIG", "卡槽配置无效");
        require(cards.size() < static_cast<size_t>(limit), "SLOTS", "银行卡槽已满");

        for (int i = 0; i < limit; ++i) {
            if (std::none_of(cards.begin(), cards.end(), [i](auto const& c) { return c.slot == i; })) {
                return i;
            }
        }

        throw Failure("SLOTS", "银行卡槽已满");
    }
};

/// 按「同服批次优先、其余 FIFO」从批次列表取走指定 base。
inline std::vector<Slice> take(std::vector<Lot>& lots, Amount amount) {
    std::vector<Slice> slices;
    for (auto& lot : lots) {
        auto n = std::min(lot.remain, amount);
        if (n > 0) {
            slices.push_back({lot.id, lot.origin, n, lot.created});
            lot.remain -= n;
            amount     -= n;
        }
        if (!amount) {
            break;
        }
    }

    require(amount == 0, "BALANCE", "银行卡余额不足");

    return slices;
}

struct WithdrawPlan {
    Amount             fee = 0;
    std::vector<Slice> principal;
    std::vector<Slice> burned;
};

/// 同服 lot 免费，异服 lot 按跨服费率计收；手续费额外从余额扣，不入任何账户。
inline WithdrawPlan withdrawPlan(std::vector<Lot> lots, Amount base, std::string const& server, Fee cross) {
    std::vector<Lot> ordered;
    for (auto const& lot : lots) {
        if (lot.origin == server) {
            ordered.push_back(lot);
        }
    }
    for (auto const& lot : lots) {
        if (lot.origin != server) {
            ordered.push_back(lot);
        }
    }

    WithdrawPlan plan;
    Amount       total     = 0;
    Amount       remaining = base;
    for (auto& lot : ordered) {
        auto n = std::min(lot.remain, remaining);
        if (n > 0) {
            plan.principal.push_back({lot.id, lot.origin, n, lot.created});
            if (lot.origin != server) {
                total = plus(total, FeePolicy::calculate(n, cross));
            }
            lot.remain -= n;
            remaining  -= n;
        }
        if (!remaining) {
            break;
        }
    }

    require(remaining == 0, "BALANCE", "银行卡余额不足");
    plan.fee = total;
    if (total > 0) {
        plan.burned = take(ordered, total);
    }

    return plan;
}

} // namespace rbank::core
