#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace rbank::core {

using Amount = std::int64_t;
using Time   = std::int64_t;
using Json   = nlohmann::json;

/// 业务失败：code 供调用方分支，消息直接呈现给玩家。
struct Failure : std::runtime_error {
    std::string code;

    Failure(std::string c, std::string const& m) : std::runtime_error(m), code(std::move(c)) {}
};

/// 三条接入通道统一返回结构。
struct Result {
    std::string code = "OK";
    std::string message;
    Json        data;

    bool ok() const { return code == "OK"; }

    static Result success(Json d = {}) { return {"OK", {}, std::move(d)}; }
    static Result fail(std::string c, std::string m) { return {std::move(c), std::move(m), {}}; }
};

/// 前置守卫：不满足即抛出，由 guarded 统一转成 Result。
inline void require(bool test, std::string const& code, std::string const& message) {
    if (!test) {
        throw Failure(code, message);
    }
}

/// 断言两数之和可表示；结果不参与计算时单独使用。
inline void fits(Amount a, Amount b) {
    require(a >= 0 && b >= 0 && a <= std::numeric_limits<Amount>::max() - b, "OVERFLOW", "金额超出范围");
}

inline Amount plus(Amount a, Amount b) {
    fits(a, b);

    return a + b;
}

inline Amount times(Amount a, Amount b) {
    require(a >= 0 && b >= 0 && (b == 0 || a <= std::numeric_limits<Amount>::max() / b), "OVERFLOW", "金额超出范围");

    return a * b;
}

/// xuid 是唯一身份，拒绝空值、"0" 与非数字串。
inline void identity(std::string const& xuid) {
    require(
        !xuid.empty() && xuid.size() <= 20 && xuid != "0" && xuid.find_first_not_of("0123456789") == std::string::npos,
        "IDENTITY",
        "需要已认证的玩家身份"
    );
}

struct Fee {
    Amount ppm   = 0;
    Amount fixed = 0;
};

struct HotConfig {
    int    slots = 1;
    Fee    deposit;
    Fee    cross;
    Fee    transfer;
    Amount maxSingle   = 1'000'000'000;
    Amount maxDaily    = 10'000'000'000;
    bool   enabled     = true;
    bool   deposits    = true;
    bool   withdrawals = true;
    bool   transfers   = true;
    bool   joinForm    = true;
};

struct Node {
    std::string id, name, currency;
    std::string version   = "0.1.0";
    std::string status    = "ONLINE";
    Time        heartbeat = 0;
};

struct Rate {
    std::string server, by;
    Amount      value     = 0;
    Time        effective = 0;
};

struct Card {
    std::string no, xuid;
    std::string status  = "ACTIVE";
    int         slot    = 0;
    Amount      balance = 0;
    Amount      version = 0;
};

struct Lot {
    std::string id, card, origin;
    Amount      original = 0;
    Amount      remain   = 0;
    Time        created  = 0;
};

/// 批次消耗快照，用于单据恢复与补偿。
struct Slice {
    std::string lot, origin;
    Amount      amount  = 0;
    Time        created = 0;
};

enum class Direction { Deposit, Withdraw, Transfer };

/// PendingLegacy 表示等待原版侧确认，PendingDb 表示原版已确认、等待入账。
enum class State { PendingLegacy, PendingDb, Settled, RolledBack, Orphan };

inline std::string name(Direction d) {
    switch (d) {
    case Direction::Deposit:
        return "DEPOSIT";
    case Direction::Withdraw:
        return "WITHDRAW";
    default:
        return "TRANSFER";
    }
}

inline std::string name(State s) {
    switch (s) {
    case State::PendingLegacy:
        return "PENDING_LEGACY";
    case State::PendingDb:
        return "PENDING_DB";
    case State::Settled:
        return "SETTLED";
    case State::RolledBack:
        return "ROLLED_BACK";
    default:
        return "ORPHAN";
    }
}

inline Direction direction(std::string const& s) {
    return s == "DEPOSIT" ? Direction::Deposit : s == "WITHDRAW" ? Direction::Withdraw : Direction::Transfer;
}

inline State state(std::string const& s) {
    if (s == "PENDING_LEGACY") {
        return State::PendingLegacy;
    }
    if (s == "PENDING_DB") {
        return State::PendingDb;
    }
    if (s == "SETTLED") {
        return State::Settled;
    }
    if (s == "ROLLED_BACK") {
        return State::RolledBack;
    }
    return State::Orphan;
}

/// 资金搬运单（Saga 日志）。
struct Intent {
    std::string        id, xuid, card, server, peer, requestKey, error;
    Direction          dir   = Direction::Deposit;
    State              state = State::PendingLegacy;
    Amount             base = 0, fee = 0, local = 0, rate = 0;
    int                attempts = 0;
    Time               created = 0, updated = 0;
    std::vector<Slice> slices;
};

struct LedgerEntry {
    std::string id, xuid, card, kind, server, peer, intent;
    Amount      delta = 0, fee = 0, balance = 0, rate = 0, local = 0;
    Time        created = 0;
};

struct AuditRecord {
    std::string           id, xuid, event, server;
    std::optional<Amount> delta;
    Amount                balance = 0;
    Time                  created = 0;
};

struct CashRequest {
    std::string xuid, card;
    Amount      local = 0;
    std::string key;
};

struct PayRequest {
    std::string xuid, card, recipient;
    Amount      base = 0;
    std::string key;
};

} // namespace rbank::core
