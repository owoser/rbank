#pragma once

#include "core/service/Policy.h"

namespace rbank::core {

/// 用例层：所有业务规则在此实现，接入适配层只做参数解析与呈现。
class Bank {
    UnitOfWork&    db;
    MoneyGateway&  money;
    Clock&         clock;
    IdGen&         ids;
    CardGenerator& cardgen;
    Node           node;

    Card   owned(Session&, std::string const&, std::string const&, bool active = true);
    void   ready(Session&, std::string const&);
    Amount rate(Session&);
    void   limits(Session&, HotConfig const&, std::string const&, Direction, Amount);
    void   ledger(Session&, Intent const&, Card const&, Amount, std::string const&, std::string const& peer = {});
    void   settleDeposit(Session&, Intent const&, State from, std::string const& note = {});
    void   undoWithdrawal(Session&, Intent const&, State from, std::string const& note = {});

public:
    Bank(UnitOfWork& d, MoneyGateway& m, Clock& c, IdGen& i, CardGenerator& g, Node n)
    : db(d),
      money(m),
      clock(c),
      ids(i),
      cardgen(g),
      node(std::move(n)) {}

    Result create(std::string const& xuid);
    Result close(std::string const& xuid, std::string const& card);
    Result list(std::string const& xuid);
    Result info(std::string const& xuid, std::string const& card);
    Result history(std::string const& xuid, int count = 10);
    Result deposit(CashRequest const&);
    Result withdraw(CashRequest const&);
    Result pay(PayRequest const&);
    Result rates();
    Result slots(std::string const& xuid);
    Result freeze(std::string const& card, bool frozen);
    Result setRate(std::string const& server, Amount value, std::string const& by);
    Result resolve(std::string const& intent, bool sourceConfirmed, std::string const& by);
    void   recover(Time grace = 60, Time offline = 300);
};

/// 出口守卫：业务失败转结果，其余异常一律按「银行不可用」呈现，不泄漏内部栈。
template <class Fn>
Result guarded(Fn&& fn) noexcept {
    try {
        return fn();
    } catch (Failure const& e) {
        return Result::fail(e.code, e.what());
    } catch (...) {
        return Result::fail("UNAVAILABLE", "银行维护中，请稍后重试");
    }
}

} // namespace rbank::core
