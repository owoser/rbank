#pragma once

#include "core/port/Ports.h"

#include <atomic>
#include <memory>

namespace rbank::infra {

/// LegacyMoney 的原版事件；回调运行在原版写库路径上，绝不能抛异常。
enum LegacyEvent { LegacySet = 0, LegacyAdd = 1, LegacyReduce = 2, LegacyTrans = 3 };

using LegacyCallback = bool (*)(LegacyEvent, std::string, std::string, long long);

/// MoneyGateway 的实现：只暴露 Get / Add / Reduce，不提供 Set。
/// 所有调用都调度到服务端主线程，并且仅在玩家在线时执行。
class LegacyMoney final : public core::MoneyGateway {
    using Read   = long long (*)(std::string);
    using Write  = bool (*)(std::string, long long);
    using Listen = void (*)(LegacyCallback);

    Read                              read{};
    Write                             addFn{};
    Write                             reduceFn{};
    Listen                            listen{};
    std::shared_ptr<std::atomic_bool> active;

    core::MoneyOutcome invoke(Write, std::string const&, core::Amount);

public:
    explicit LegacyMoney(std::shared_ptr<std::atomic_bool>);

    std::optional<core::Amount> get(std::string const&) override;
    core::MoneyOutcome          add(std::string const& xuid, core::Amount n) override { return invoke(addFn, xuid, n); }
    core::MoneyOutcome reduce(std::string const& xuid, core::Amount n) override { return invoke(reduceFn, xuid, n); }

    void subscribe(LegacyCallback callback) { listen(callback); }

    /// 主线程读取余额；离线返回 -1。审计旁路专用。
    core::Amount balance(std::string const& xuid) { return read(xuid); }
};

} // namespace rbank::infra
