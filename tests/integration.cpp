// 集成自检：跑真实 MySQL，用可编程的假网关替代 LegacyMoney。
// 凭据由环境变量注入（MYSQL_ADDRESS / MYSQL_PORT / MYSQL_USER / MYSQL_PASS），不写进源码。
#include "bootstrap/config/Config.h"
#include "core/service/Bank.h"
#include "infra/cardgen/CardGenerator.h"
#include "infra/mysql/Database.h"
#include "infra/runtime/Clock.h"

#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>

namespace {

using namespace rbank::core;

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
    } catch (Failure const& e) {
        return e.code;
    } catch (std::exception const& e) {
        std::cerr << "  unexpected: " << e.what() << '\n';
        return "OTHER";
    }

    return "OK";
}

/// 原版侧的可编程替身：记录调用顺序，可切换为成功 / 拒绝 / 结果不明。
struct FakeMoney final : MoneyGateway {
    std::mutex               mutex;
    Amount                   balance = 1'000'000;
    std::vector<std::string> calls;
    MoneyOutcome             mode = MoneyOutcome::Success;

    std::optional<Amount> get(std::string const&) override {
        std::lock_guard lock(mutex);

        return balance;
    }

    MoneyOutcome add(std::string const&, Amount n) override {
        std::lock_guard lock(mutex);
        calls.push_back("add:" + std::to_string(n));
        if (mode == MoneyOutcome::Failed) {
            return MoneyOutcome::Failed;
        }
        if (mode == MoneyOutcome::Unknown) {
            return MoneyOutcome::Unknown;
        }
        balance += n;

        return MoneyOutcome::Success;
    }

    MoneyOutcome reduce(std::string const&, Amount n) override {
        std::lock_guard lock(mutex);
        calls.push_back("reduce:" + std::to_string(n));
        if (mode == MoneyOutcome::Failed || balance < n) {
            return MoneyOutcome::Failed;
        }
        if (mode == MoneyOutcome::Unknown) {
            return MoneyOutcome::Unknown;
        }
        balance -= n;

        return MoneyOutcome::Success;
    }

    int count(std::string const& prefix) {
        std::lock_guard lock(mutex);

        return static_cast<int>(std::count_if(calls.begin(), calls.end(), [&](auto& c) {
            return c.rfind(prefix, 0) == 0;
        }));
    }

    void reset() {
        std::lock_guard lock(mutex);
        calls.clear();
        mode = MoneyOutcome::Success;
    }
};

std::string env(char const* name, std::string fallback = {}) {
    char*  value = nullptr;
    size_t len   = 0;
    _dupenv_s(&value, &len, name);
    std::string result = value ? value : fallback;
    std::free(value);

    return result;
}

Amount lotsTotal(Session& s, std::string const& no) {
    Amount sum = 0;
    for (auto const& lot : s.lots(no)) {
        sum += lot.remain;
    }

    return sum;
}

/// 测试库每次从空表开始；否则上一轮的单据会挡住本轮（unsettled / 唯一键）。
void reset(rbank::infra::Database& db) {
    auto s = db.open();
    s->begin();
    for (auto const& table :
         {"ledger", "audit_legacy", "intent", "card_lot", "card", "rate", "config", "card_gen_seq", "node"}) {
        s->execute(std::string("DELETE FROM ") + table);
    }
    s->commit();
}

} // namespace

int main() {
    using namespace rbank;

    infra::DatabaseConfig config;
    config.host              = env("MYSQL_ADDRESS");
    config.port              = static_cast<unsigned>(std::stoul(env("MYSQL_PORT", "3306")));
    config.user              = env("MYSQL_USER", "root");
    config.password          = env("MYSQL_PASS");
    config.database          = env("MYSQL_DATABASE", "rbank_test");
    config.library           = env("MYSQL_LIBRARY", "build/deps/mariadb/libmariadb.dll");
    config.pool              = 4;
    config.verifyCertificate = env("MYSQL_VERIFY", "1") != "0";

    bootstrap::CardConfig cardCfg;
    infra::Database       db(config);
    infra::SystemClock    clock;
    infra::RandomId       ids;
    infra::CardGenerator  generator(cardCfg);
    FakeMoney             money;

    Node node{"test-a", "测试服 A", "金币"};
    node.heartbeat = clock.now();
    Bank bank(db, money, clock, ids, generator, node);


    try {
        db.migrate("assets/schema/migrations");
        reset(db);
    } catch (std::exception const& e) {
        std::cerr << "migrate failed: " << e.what() << '\n';

        return 1;
    }

    {
        auto        s = db.open();
        Transaction tx(*s);
        check(s->registerNode(node), "register node");
        s->heartbeat(node);
        s->appendRate({node.id, "test", 1, clock.now() - 10});
        HotConfig hot;
        hot.slots     = 2;
        hot.maxSingle = 1'000'000;
        hot.maxDaily  = 10'000'000;
        hot.deposit   = {0, 0};
        hot.cross     = {10'000, 0};
        hot.transfer  = {10'000, 0};
        s->putConfig(hot, "test");
        tx.commit();
    }


    auto opened = db.open()->nodes();
    check(!opened.empty() && opened[0].status == "ONLINE", "node online");

    // ---------- 开卡 ----------
    std::string cardA, cardB;
    check(code([&] { cardA = bank.create("1000000000000001").data.at("card_no"); }) == "OK", "create card A");
    check(code([&] { cardB = bank.create("1000000000000002").data.at("card_no"); }) == "OK", "create card B");
    check(cardA != cardB, "card numbers unique");
    check(code([&] { bank.create("1000000000000001"); }) == "OK", "second slot for A");
    check(code([&] { bank.create("1000000000000001"); }) == "SLOTS", "third card rejected by slot limit");
    check(bank.list("1000000000000001").data.size() == 2, "list returns two cards for A");

    // ---------- 存款：成功路径 ----------
    money.reset();
    auto deposit = bank.deposit({"1000000000000001", cardA, 500, "k-deposit-1"});
    check(deposit.ok(), "deposit ok");
    check(money.count("reduce:500") == 1, "deposit reduces legacy once");
    check(money.balance == 1'000'000 - 500, "legacy balance reduced");
    {
        auto s    = db.open();
        auto card = s->card(cardA);
        check(card && card->balance == 500, "card credited 500 base");
        check(lotsTotal(*s, cardA) == 500, "lots match balance");
        auto lots = s->lots(cardA);
        check(lots.size() == 1 && lots[0].origin == "test-a", "lot carries origin server");
    }

    // ---------- 存款：原版拒绝 ----------
    money.reset();
    money.mode = MoneyOutcome::Failed;
    check(code([&] { bank.deposit({"1000000000000002", cardB, 500, {}}); }) == "LEGACY", "deposit rejected by legacy");
    check(db.open()->card(cardB)->balance == 0, "failed deposit leaves balance untouched");
    check(money.count("add:") == 0, "failed deposit never compensates");

    // ---------- 存款：结果不明不重放 ----------
    money.reset();
    money.mode = MoneyOutcome::Unknown;
    check(code([&] { bank.deposit({"1000000000000002", cardB, 500, {}}); }) == "PENDING", "unknown deposit reported");
    check(db.open()->card(cardB)->balance == 0, "unknown deposit does not credit");
    check(money.count("reduce:") == 1 && money.count("add:") == 0, "unknown deposit never replayed");

    // 结果不明的单据会挂住该 xuid；宽限期过后由恢复流程终结（少给方向）。
    bank.recover(0, 300);
    {
        auto s = db.open();
        check(!s->unsettled("1000000000000002"), "recovery clears unsettled intent");
    }
    money.mode = MoneyOutcome::Success;

    // ---------- 取款：同服批次免费 ----------
    money.reset();
    auto withdraw = bank.withdraw({"1000000000000001", cardA, 200, {}});
    check(withdraw.ok(), "withdraw ok");
    check(withdraw.data.at("fee_base").get<Amount>() == 0, "same-server withdrawal free");
    check(money.count("add:200") == 1, "withdraw adds to legacy");
    {
        auto s    = db.open();
        auto card = s->card(cardA);
        check(card->balance == 300, "balance after withdraw");
        check(lotsTotal(*s, cardA) == 300, "lots after withdraw");
    }

    // ---------- 取款：原版到账失败 → 补偿 ----------
    money.reset();
    money.mode = MoneyOutcome::Failed;
    check(code([&] { bank.withdraw({"1000000000000001", cardA, 100, {}}); }) == "LEGACY", "withdraw add failure");
    {
        auto s    = db.open();
        auto card = s->card(cardA);
        check(card->balance == 300, "compensation restores balance");
        check(lotsTotal(*s, cardA) == 300, "compensation restores lots");
        auto history = s->history("1000000000000001", 5);
        check(!history.empty() && history[0].kind == "WITHDRAW_ROLLBACK", "rollback recorded in ledger");
    }
    money.mode = MoneyOutcome::Success;

    // ---------- 跨服批次计费 ----------
    // 异服批次的外键要求来源服已在 node 表中注册。
    {
        Node peer{"test-b", "测试服 B", "金币"};
        peer.heartbeat = clock.now();
        auto        s  = db.open();
        Transaction tx(*s);
        s->registerNode(peer);
        s->heartbeat(peer);
        tx.commit();
    }

    std::string cardC;
    check(code([&] { cardC = bank.create("1000000000000003").data.at("card_no"); }) == "OK", "create card C");
    {
        auto        s = db.open({});
        Transaction tx(*s);
        Lot         lot;
        lot.id       = ids.next();
        lot.card     = cardC;
        lot.origin   = "test-b";
        lot.original = 100;
        lot.remain   = 100;
        lot.created  = clock.now();
        s->insertLot(lot);
        auto card     = s->card(cardC);
        card->balance = 100;
        ++card->version;
        s->saveCard(*card);
        tx.commit();
    }
    money.reset();
    auto cross = bank.withdraw({"1000000000000003", cardC, 40, {}});
    check(cross.ok(), "cross withdrawal ok");
    check(cross.data.at("fee_base").get<Amount>() == 1, "cross fee ceil(1%) = 1");
    check(money.count("add:40") == 1, "cross withdrawal pays requested local amount");
    {
        auto s = db.open();
        check(s->card(cardC)->balance == 59, "fee destroyed from balance");
        check(lotsTotal(*s, cardC) == 59, "fee destroyed from lots");
    }

    // ---------- 转账保留来源服 ----------
    money.reset();
    Result pay;
    check(code([&] { pay = bank.pay({"1000000000000001", cardA, cardB, 100, {}}); }) == "OK", "pay ok");
    check(pay.data.at("fee_base").get<Amount>() == 1, "transfer fee 1%");
    {
        auto s = db.open();
        check(s->card(cardA)->balance == 199, "payer balance after transfer");
        check(s->card(cardB)->balance == 100, "payee balance after transfer");
        auto lots = s->lots(cardB);
        check(lots.size() == 1 && lots[0].origin == "test-a", "transferred lot keeps origin");
        check(money.calls.empty(), "internal transfer never touches legacy");
    }

    // ---------- 幂等 ----------
    money.reset();
    check(code([&] { bank.deposit({"1000000000000001", cardA, 10, "k-shared"}); }) == "OK", "keyed deposit first time");
    check(
        code([&] { bank.deposit({"1000000000000001", cardA, 10, "k-shared"}); }) == "DUPLICATE",
        "repeated key rejected"
    );
    check(db.open()->card(cardA)->balance == 209, "repeated key credits once");
    check(money.count("reduce:") == 1, "repeated key reduces once");

    // ---------- 限额 ----------
    check(code([&] { bank.deposit({"1000000000000001", cardA, 2'000'000, {}}); }) == "LIMIT", "single limit");

    // 日限额按「已用 + 本笔」判定，单笔限额必须同步压低才能触发。
    auto setLimits = [&](Amount single, Amount daily) {
        auto result = code([&] {
            auto      s   = db.open();
            HotConfig hot = s->config();
            hot.maxSingle = single;
            hot.maxDaily  = daily;
            Transaction tx(*s);
            s->putConfig(hot, "test");
            tx.commit();
        });
        std::cerr << "  setLimits(" << single << ',' << daily << ") => " << result << '\n';
        check(result == "OK", "set limits");
    };
    // 日限额按当日全部单据求和，因此用全新玩家验证，避免受前面用例的累计影响。
    setLimits(100, 150);
    std::string cardD;
    check(code([&] { cardD = bank.create("1000000000000004").data.at("card_no"); }) == "OK", "create card D");
    check(code([&] { bank.deposit({"1000000000000004", cardD, 100, {}}); }) == "OK", "first deposit under daily limit");
    check(code([&] { bank.deposit({"1000000000000004", cardD, 100, {}}); }) == "LIMIT", "daily limit");
    check(code([&] { bank.withdraw({"1000000000000004", cardD, 100, {}}); }) == "OK", "withdraw within balance");
    setLimits(1'000'000, 10'000'000);

    // ---------- 并发：同 xuid 必须串行 ----------
    money.reset();
    std::vector<std::string> results(4);
    {
        std::vector<std::jthread> threads;
        for (int i = 0; i < 4; ++i) {
            threads.emplace_back([&, i] {
                results[static_cast<size_t>(i)] =
                    code([&] { bank.deposit({"1000000000000001", cardA, 10, "k-concurrent-" + std::to_string(i)}); });
            });
        }
    }
    check(std::all_of(results.begin(), results.end(), [](auto& r) { return r == "OK"; }), "concurrent deposits all ok");
    {
        auto s = db.open();
        check(s->card(cardA)->balance == 209 + 40, "no lost update under concurrency");
        check(lotsTotal(*s, cardA) == 249, "lots consistent under concurrency");
    }

    // ---------- 恢复：本服 PENDING_LEGACY 终结 ----------
    std::string stale;
    {
        auto        s    = db.open();
        auto        card = s->card(cardA);
        Transaction tx(*s);
        Intent      intent;
        intent.id = stale = ids.next();
        intent.xuid       = "1000000000000001";
        intent.card       = cardA;
        intent.server     = "test-a";
        intent.state      = State::PendingLegacy;
        intent.base       = 50;
        intent.local      = 50;
        intent.rate       = 1;
        intent.created    = clock.now() - 7200;
        intent.updated    = intent.created;
        s->insertIntent(intent);
        tx.commit();
    }
    bank.recover(60, 300);
    check(db.open()->intent(stale)->state == State::RolledBack, "stale pending legacy rolled back");

    // ---------- 恢复：本服 PENDING_DB 前滚入账 ----------
    std::string rolling;
    Amount      before = db.open()->card(cardA)->balance;
    {
        auto        s = db.open();
        Transaction tx(*s);
        Intent      intent;
        intent.id = rolling = ids.next();
        intent.xuid         = "1000000000000001";
        intent.card         = cardA;
        intent.server       = "test-a";
        intent.state        = State::PendingDb;
        intent.base         = 70;
        intent.fee          = 0;
        intent.local        = 70;
        intent.rate         = 1;
        intent.created      = clock.now() - 7200;
        intent.updated      = intent.created;
        s->insertIntent(intent);
        tx.commit();
    }
    bank.recover(60, 300);
    {
        auto s = db.open();
        check(s->intent(rolling)->state == State::Settled, "pending db rolled forward");
        check(s->card(cardA)->balance == before + 70, "roll forward credits balance");
        check(lotsTotal(*s, cardA) == before + 70, "roll forward adds lot");
    }

    // ---------- 删卡守卫与冻结 ----------
    check(code([&] { bank.close("1000000000000001", cardA); }) == "BALANCE", "cannot close card with balance");
    check(bank.freeze(cardA, true).data.at("status") == "FROZEN", "freeze works");
    check(code([&] { bank.deposit({"1000000000000001", cardA, 10, {}}); }) == "FROZEN", "frozen card rejects deposit");
    check(bank.freeze(cardA, false).data.at("status") == "ACTIVE", "unfreeze works");

    // 未决单据会挡住该 xuid 的后续资金操作，直到恢复流程终结它。
    {
        auto        s = db.open();
        Transaction tx(*s);
        Intent      stuck;
        stuck.id      = ids.next();
        stuck.xuid    = "1000000000000002";
        stuck.card    = cardB;
        stuck.server  = "test-a";
        stuck.state   = State::PendingLegacy;
        stuck.base    = 10;
        stuck.local   = 10;
        stuck.rate    = 1;
        stuck.created = clock.now();
        stuck.updated = stuck.created;
        s->insertIntent(stuck);
        tx.commit();
    }
    check(
        code([&] { bank.deposit({"1000000000000002", cardB, 10, {}}); }) == "PENDING",
        "unsettled intent blocks xuid"
    );
    bank.recover(0, 300);
    check(code([&] { bank.deposit({"1000000000000002", cardB, 10, {}}); }) == "OK", "recovery unblocks xuid");

    // ---------- 汇率校验 ----------
    check(code([&] { bank.setRate("test-a", 0, "test"); }) == "RATE", "rate must be positive");
    check(code([&] { bank.setRate("missing", 1, "test"); }) == "NODE", "unknown node rejected");

    std::cout << (failed ? "INTEGRATION FAILED\n" : "INTEGRATION OK\n");

    return failed ? 1 : 0;
}
