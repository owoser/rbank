#pragma once

#include "core/model/Model.h"

#include <memory>
#include <string>
#include <vector>

namespace rbank::core {

struct CardRepo {
    virtual std::optional<Card> card(std::string const&)  = 0;
    virtual std::vector<Card>   cards(std::string const&) = 0;
    virtual bool                insertCard(Card const&)   = 0;
    virtual void                saveCard(Card const&)     = 0;
    virtual ~CardRepo()                                   = default;
};

struct LotRepo {
    virtual std::vector<Lot> lots(std::string const&)               = 0;
    virtual void             insertLot(Lot const&)                  = 0;
    virtual void             consumeLot(std::string const&, Amount) = 0;
    virtual ~LotRepo()                                              = default;
};

struct LedgerRepo {
    virtual void                     appendLedger(LedgerEntry const&)           = 0;
    virtual std::vector<LedgerEntry> history(std::string const&, int)           = 0;
    virtual Amount                   daily(std::string const&, Direction, Time) = 0;
    virtual ~LedgerRepo()                                                       = default;
};

struct IntentRepo {
    virtual void                  insertIntent(Intent const&)                                                = 0;
    virtual std::optional<Intent> intent(std::string const&)                                                 = 0;
    virtual bool                  cas(std::string const&, State, State, Time, std::string const& error = {}) = 0;
    virtual bool                  unsettled(std::string const&)                                              = 0;
    virtual std::vector<Intent>   pending(Time)                                                              = 0;
    virtual ~IntentRepo()                                                                                    = default;
};

struct NodeRepo {
    virtual std::vector<Node> nodes()                   = 0;
    virtual bool              registerNode(Node const&) = 0;
    virtual void              heartbeat(Node const&)    = 0;
    virtual ~NodeRepo()                                 = default;
};

struct RateRepo {
    virtual std::vector<Rate> rates(Time)             = 0;
    virtual void              appendRate(Rate const&) = 0;
    virtual ~RateRepo()                               = default;
};

struct ConfigStore {
    virtual HotConfig config()                                        = 0;
    virtual void      putConfig(HotConfig const&, std::string const&) = 0;
    virtual ~ConfigStore()                                            = default;
};

struct AuditRepo {
    virtual void appendAudit(AuditRecord const&) = 0;
    virtual ~AuditRepo()                         = default;
};

/// 一个 Session 独占一条池化连接，并在连接作用域内持有 xuid 互斥锁。
/// 连接不可重连，也不得借给另一个 Session。
struct Session : CardRepo, LotRepo, LedgerRepo, IntentRepo, NodeRepo, RateRepo, ConfigStore, AuditRepo {
    virtual void   begin()                      = 0;
    virtual void   commit()                     = 0;
    virtual void   rollback() noexcept          = 0;
    virtual Amount sequence(std::string const&) = 0;

    /// 仅供运维工具使用的直通执行；业务代码不得调用。
    virtual void execute(std::string const&) = 0;
    virtual ~Session()                       = default;
};

struct UnitOfWork {
    virtual std::unique_ptr<Session> open(std::vector<std::string> xuids = {}) = 0;
    virtual ~UnitOfWork()                                                      = default;
};

/// 事务守卫：未 commit 即回滚。
struct Transaction {
    Session& s;
    bool     done = false;

    explicit Transaction(Session& session) : s(session) { s.begin(); }

    void commit() {
        s.commit();
        done = true;
    }

    ~Transaction() {
        if (!done) {
            s.rollback();
        }
    }
};

/// 原版接口没有事务，只能报告三种结果；Unknown 绝不能自动重放。
enum class MoneyOutcome { Success, Failed, Unknown };

struct MoneyGateway {
    virtual std::optional<Amount> get(std::string const&)            = 0;
    virtual MoneyOutcome          add(std::string const&, Amount)    = 0;
    virtual MoneyOutcome          reduce(std::string const&, Amount) = 0;
    virtual ~MoneyGateway()                                          = default;
};

struct Clock {
    virtual Time now() = 0;
    virtual ~Clock()   = default;
};

struct IdGen {
    virtual std::string next() = 0;
    virtual ~IdGen()           = default;
};

struct CardGenerator {
    virtual std::string generate(std::string const&, Session&) = 0;
    virtual ~CardGenerator()                                   = default;
};

} // namespace rbank::core
