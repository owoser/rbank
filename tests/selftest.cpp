// 宿主侧自检：不依赖 MC / MySQL，只验证货物层与纯规则。
#include "bootstrap/config/Config.h"
#include "core/service/Policy.h"
#include "infra/cardgen/CardGenerator.h"
#include "proto/Protocol.h"

#include <iostream>

namespace {

int failed = 0;

void check(bool ok, std::string const& what) {
    if (!ok) {
        ++failed;
        std::cerr << "FAIL " << what << '\n';
    }
}

template <class Fn>
std::string code(Fn&& fn) {
    try {
        fn();
    } catch (rbank::core::Failure const& e) {
        return e.code;
    } catch (...) {
        return "OTHER";
    }

    return "OK";
}

} // namespace

int main() {
    using namespace rbank::core;

    // 汇率折算：非整倍必须拒绝，且提示实际倍数。
    check(FeePolicy::convert(1000, 100) == 10, "convert ok");
    check(code([] { FeePolicy::convert(1050, 100); }) == "MULTIPLE", "convert rejects non-multiple");
    check(code([] { FeePolicy::convert(100, 0); }) == "RATE", "convert rejects zero rate");
    check(code([] { FeePolicy::convert(0, 100); }) == "AMOUNT", "convert rejects zero amount");

    // 手续费：向上取整 + 固定值，且极端值不溢出。
    check(FeePolicy::calculate(0, {0, 0}) == 0, "fee zero");
    check(FeePolicy::calculate(1000, {10'000, 0}) == 10, "fee exact");
    check(FeePolicy::calculate(1001, {10'000, 0}) == 11, "fee ceil");
    check(FeePolicy::calculate(1, {1, 0}) == 1, "fee minimum one");
    check(FeePolicy::calculate(1, {1, 5}) == 6, "fee fixed");
    check(FeePolicy::calculate(std::numeric_limits<Amount>::max(), {1'000'000, 0}) > 0, "fee no overflow");

    // 取款计费：同服批次免费，异服批次按跨服费率，手续费另从余额扣。
    std::vector<Lot> lots = {
        {"l1", "C1", "B", 60, 60, 1},
        {"l2", "C1", "A", 40, 40, 2}
    };
    auto same = withdrawPlan(lots, 50, "B", {10'000, 0});
    check(same.fee == 0, "same-server fee free");
    check(same.principal.size() == 1 && same.principal[0].lot == "l1", "same-server lot used first");
    check(same.burned.empty(), "same-server nothing burned");

    std::vector<Lot> mixed = {
        {"l1", "C1", "A", 60, 60, 1},
        {"l2", "C1", "B", 40, 40, 2}
    };
    // 80 里 40 来自 B 服批次（免费），40 来自 A 服（1% 向上取整 = 1）。
    auto cross = withdrawPlan(mixed, 80, "B", {10'000, 0});
    check(cross.fee == 1, "cross fee charged on foreign slice only");
    check(cross.burned.size() == 1 && cross.burned[0].amount == 1, "cross fee burned from balance");
    check(code([&] { withdrawPlan(mixed, 101, "B", {10'000, 0}); }) == "BALANCE", "withdraw over balance");

    // 批次顺序：同服优先，即便它在 FIFO 之后。
    std::vector<Lot> ordered = {
        {"old", "C1", "A", 10, 10, 1},
        {"new", "C1", "B", 10, 10, 2}
    };
    auto plan = withdrawPlan(ordered, 10, "B", {0, 0});
    check(plan.principal.size() == 1 && plan.principal[0].lot == "new", "same-server lot preferred");

    // 卡号规则校验：总长、字母表、支票位类型。
    using rbank::infra::CardGenerator;
    rbank::bootstrap::CardConfig cfg;
    check(code([&] { CardGenerator good(cfg); }) == "OK", "default card config valid");
    check(
        code([] {
            rbank::bootstrap::CardConfig bad;
            bad.length = 99;
            CardGenerator rejected(bad);
        }) == "CONFIG",
        "card length rejected"
    );
    check(
        code([] {
            rbank::bootstrap::CardConfig bad;
            bad.alphabet = "0011";
            CardGenerator rejected(bad);
        }) == "CONFIG",
        "duplicate alphabet rejected"
    );
    check(
        code([] {
            rbank::bootstrap::CardConfig bad;
            bad.checksum = "LUHN10";
            bad.fields   = {"RANDOM_ALNUM:10"};
            bad.length   = 11;
            CardGenerator rejected(bad);
        }) == "CONFIG",
        "LUHN10 rejects letters"
    );

    // 热配置编解码往返，并拒绝非法费率。
    auto json = rbank::bootstrap::hotJson(HotConfig{});
    check(rbank::bootstrap::hotConfig(json).slots == HotConfig{}.slots, "hot config round trip");
    check(
        code([&] {
            auto bad                  = json;
            bad["deposit_fee"]["ppm"] = -1;
            rbank::bootstrap::hotConfig(bad);
        }) == "CONFIG",
        "negative fee rejected"
    );
    check(
        code([&] {
            auto bad              = json;
            bad["max_daily_base"] = 1;
            rbank::bootstrap::hotConfig(bad);
        }) == "CONFIG",
        "daily below single rejected"
    );
    check(
        code([&] {
            auto bad     = json;
            bad["slots"] = 101;
            rbank::bootstrap::hotConfig(bad);
        }) == "CONFIG",
        "slot overflow rejected"
    );

    // 汇率广播：首次静默，之后只报变化。
    rbank::proto::RateBroadcast broadcast;
    std::vector<Rate>           initial{
                  {"A", "ops", 100, 1},
                  {"B", "ops", 10,  1}
    };
    check(broadcast.changes(initial).empty(), "broadcast primed silently");
    std::vector<Rate> changed{
        {"A", "ops", 100, 1},
        {"B", "ops", 20,  1}
    };
    auto diff = broadcast.changes(changed);
    check(diff.size() == 1 && diff[0].server == "B", "broadcast only changed server");

    std::cout << (failed ? "SELFTEST FAILED\n" : "SELFTEST OK\n");

    return failed ? 1 : 0;
}
