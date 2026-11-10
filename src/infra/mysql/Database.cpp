#include "infra/mysql/Database.h"

#include "bootstrap/config/Config.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <random>
#include <sstream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace rbank::infra {

using namespace core;

/// 官方 MySQL C API 的 ABI：只持有不透明句柄，不触碰 MYSQL 结构体布局。
struct Database::Api {
    HMODULE module = nullptr;

    void* (*init)(void*) = nullptr;
    void* (*connect)(void*, char const*, char const*, char const*, char const*, unsigned, char const*, unsigned long) =
        nullptr;
    int (*options)(void*, int, void const*)         = nullptr;
    void (*close)(void*)                            = nullptr;
    int (*query)(void*, char const*, unsigned long) = nullptr;
    void* (*storeResult)(void*)                     = nullptr;
    char** (*fetch)(void*)                          = nullptr;
    unsigned long* (*lengths)(void*)                = nullptr;
    unsigned (*fields)(void*)                       = nullptr;
    void (*freeResult)(void*)                       = nullptr;
    unsigned (*errorNumber)(void*)                  = nullptr;
    char const* (*errorMessage)(void*)              = nullptr;
    unsigned long long (*affectedRows)(void*)       = nullptr;
    unsigned char (*threadInit)()                   = nullptr;
    void (*threadEnd)()                             = nullptr;

    explicit Api(std::string const& library) {
        module = LoadLibraryA(library.c_str());
        require(module != nullptr, "DB_LIBRARY", "无法加载数据库客户端库: " + library);

#define RBANK_LOAD(member, symbol)                                                                                     \
    member = reinterpret_cast<decltype(member)>(GetProcAddress(module, symbol));                                       \
    require(member != nullptr, "DB_LIBRARY", "数据库客户端库缺少导出: " symbol)
        RBANK_LOAD(init, "mysql_init");
        RBANK_LOAD(connect, "mysql_real_connect");
        RBANK_LOAD(options, "mysql_options");
        RBANK_LOAD(close, "mysql_close");
        RBANK_LOAD(query, "mysql_real_query");
        RBANK_LOAD(storeResult, "mysql_store_result");
        RBANK_LOAD(fetch, "mysql_fetch_row");
        RBANK_LOAD(lengths, "mysql_fetch_lengths");
        RBANK_LOAD(fields, "mysql_num_fields");
        RBANK_LOAD(freeResult, "mysql_free_result");
        RBANK_LOAD(errorNumber, "mysql_errno");
        RBANK_LOAD(errorMessage, "mysql_error");
        RBANK_LOAD(affectedRows, "mysql_affected_rows");
        RBANK_LOAD(threadInit, "mysql_thread_init");
        RBANK_LOAD(threadEnd, "mysql_thread_end");
#undef RBANK_LOAD
    }

    ~Api() {
        if (module) {
            FreeLibrary(module);
        }
    }
};

namespace {

/// 客户端库要求每个使用它的线程做一次初始化/收尾。
struct ThreadContext {
    std::shared_ptr<Database::Api> api;

    explicit ThreadContext(std::shared_ptr<Database::Api> a) : api(std::move(a)) {
        require(api->threadInit() == 0, "DB", "无法初始化数据库线程上下文");
    }
    ~ThreadContext() { api->threadEnd(); }
};

using Rows = std::vector<std::vector<std::string>>;

/// 字符串一律以十六进制字面量拼入，杜绝引号与转义注入。
std::string quote(std::string const& value) {
    if (value.empty()) {
        return "''";
    }

    static constexpr char hex[] = "0123456789abcdef";
    std::string           out   = "CONVERT(X'";
    for (unsigned char c : value) {
        out += hex[c >> 4];
        out += hex[c & 15];
    }

    return out + "' USING utf8mb4)";
}

std::string num(Amount n) { return std::to_string(n); }

Amount integer(std::string const& s) {
    Amount n{};
    auto [p, e] = std::from_chars(s.data(), s.data() + s.size(), n);
    require(e == std::errc{} && p == s.data() + s.size(), "DB", "数据库返回了非法整数");

    return n;
}

class MysqlSession final : public Session {
    Database&                pool;
    void*                    connection;
    bool                     broken      = false;
    bool                     transaction = false;
    std::vector<std::string> locks;

    Rows sql(std::string const& statement);

    std::string forUpdate() const { return transaction ? " FOR UPDATE" : ""; }

    static Intent parseIntent(std::vector<std::string> const& r);

public:
    MysqlSession(Database&, std::vector<std::string> xuids);
    ~MysqlSession() override;

    void   begin() override;
    void   commit() override;
    void   rollback() noexcept override;
    Amount sequence(std::string const&) override;

    std::optional<Card> card(std::string const&) override;
    std::vector<Card>   cards(std::string const&) override;
    bool                insertCard(Card const&) override;
    void                saveCard(Card const&) override;

    std::vector<Lot> lots(std::string const&) override;
    void             insertLot(Lot const&) override;
    void             consumeLot(std::string const&, Amount) override;

    void                     appendLedger(LedgerEntry const&) override;
    std::vector<LedgerEntry> history(std::string const&, int) override;
    Amount                   daily(std::string const&, Direction, Time) override;

    void                  insertIntent(Intent const&) override;
    std::optional<Intent> intent(std::string const&) override;
    bool                  cas(std::string const&, State, State, Time, std::string const& error) override;
    bool                  unsettled(std::string const&) override;
    std::vector<Intent>   pending(Time) override;

    std::vector<Node> nodes() override;
    bool              registerNode(Node const&) override;
    void              heartbeat(Node const&) override;

    std::vector<Rate> rates(Time) override;
    void              appendRate(Rate const&) override;

    HotConfig config() override;
    void      putConfig(HotConfig const&, std::string const&) override;

    void appendAudit(AuditRecord const&) override;

    void migrate(std::filesystem::path const&);
};

} // namespace

Rows MysqlSession::sql(std::string const& statement) {
    if (pool.client()->query(connection, statement.c_str(), static_cast<unsigned long>(statement.size()))) {
        auto code = pool.client()->errorNumber(connection);
        // 2006/2013/2055 都是连接级故障：该连接上的锁已失效，必须丢弃而非复用。
        if (code == 2006 || code == 2013 || code == 2055) {
            broken = true;
        }
        if (code == 1062) {
            throw Failure("DUPLICATE", "该操作已处理，或卡号已存在");
        }

        throw std::runtime_error("MySQL " + std::to_string(code) + ": " + pool.client()->errorMessage(connection));
    }

    Rows rows;
    auto result = pool.client()->storeResult(connection);
    if (!result) {
        if (pool.client()->errorNumber(connection)) {
            broken = true;
            throw std::runtime_error("MySQL 结果集读取失败");
        }

        return rows;
    }

    try {
        auto columns = pool.client()->fields(result);
        while (auto row = pool.client()->fetch(result)) {
            auto                     lengths = pool.client()->lengths(result);
            std::vector<std::string> values;
            for (unsigned i = 0; i < columns; ++i) {
                values.emplace_back(row[i] ? std::string(row[i], lengths[i]) : "");
            }
            rows.push_back(std::move(values));
        }
    } catch (...) {
        pool.client()->freeResult(result);
        throw;
    }

    pool.client()->freeResult(result);

    return rows;
}

Intent MysqlSession::parseIntent(std::vector<std::string> const& r) {
    Intent i;
    i.id         = r[0];
    i.xuid       = r[1];
    i.dir        = direction(r[2]);
    i.card       = r[3];
    i.base       = integer(r[4]);
    i.fee        = integer(r[5]);
    i.local      = integer(r[6]);
    i.rate       = integer(r[7]);
    i.server     = r[8];
    i.state      = state(r[9]);
    i.attempts   = static_cast<int>(integer(r[10]));
    i.error      = r[11];
    i.requestKey = r[12];
    i.peer       = r[13];
    i.created    = integer(r[15]);
    i.updated    = integer(r[16]);

    for (auto const& slice : Json::parse(r[14])) {
        i.slices.push_back({slice.at("lot"), slice.at("origin"), slice.at("amount"), slice.at("created")});
    }

    return i;
}

namespace {

constexpr char const* intentColumns =
    "intent_id,xuid,dir,card_no,base_amount,fee_base,local_amount,rate,server_id,state,"
    "attempts,error,request_key,peer_xuid,lot_snapshot,created_at,updated_at";

} // namespace

MysqlSession::MysqlSession(Database& p, std::vector<std::string> xuids) : pool(p), connection(p.acquire()) {
    try {
        // 锁按字典序获取，多方操作不会交叉死锁。
        std::sort(xuids.begin(), xuids.end());
        xuids.erase(std::unique(xuids.begin(), xuids.end()), xuids.end());

        for (auto const& xuid : xuids) {
            identity(xuid);
            auto key  = "rbank:xuid:" + xuid;
            auto rows = sql("SELECT GET_LOCK(" + quote(key) + ",2)");
            require(!rows.empty() && rows[0][0] == "1", "BUSY", "资金操作繁忙，请稍后重试");
            locks.push_back(key);
        }
    } catch (...) {
        for (auto const& key : locks) {
            try {
                sql("SELECT RELEASE_LOCK(" + quote(key) + ")");
            } catch (...) {}
        }
        broken = true;
        pool.release(connection, broken);
        throw;
    }
}

MysqlSession::~MysqlSession() {
    rollback();
    for (auto const& key : locks) {
        try {
            sql("SELECT RELEASE_LOCK(" + quote(key) + ")");
        } catch (...) {
            broken = true;
        }
    }

    pool.release(connection, broken);
}

void MysqlSession::begin() {
    require(!transaction, "TRANSACTION", "事务已开启");
    sql("START TRANSACTION");
    transaction = true;
}

void MysqlSession::commit() {
    sql("COMMIT");
    transaction = false;
}

void MysqlSession::rollback() noexcept {
    if (!transaction) {
        return;
    }

    try {
        sql("ROLLBACK");
    } catch (...) {
        broken = true;
    }
    transaction = false;
}

std::optional<Card> MysqlSession::card(std::string const& no) {
    auto rows =
        sql("SELECT card_no,xuid,slot_index,balance_base,version,status FROM card WHERE card_no=" + quote(no)
            + forUpdate());
    if (rows.empty()) {
        return {};
    }

    auto const& r = rows[0];

    return Card{r[0], r[1], r[5], static_cast<int>(integer(r[2])), integer(r[3]), integer(r[4])};
}

std::vector<Card> MysqlSession::cards(std::string const& xuid) {
    std::vector<Card> cards;
    for (auto const& r :
         sql("SELECT card_no,xuid,slot_index,balance_base,version,status FROM card WHERE xuid=" + quote(xuid)
             + " AND status<>'CLOSED' ORDER BY slot_index" + forUpdate())) {
        cards.push_back({r[0], r[1], r[5], static_cast<int>(integer(r[2])), integer(r[3]), integer(r[4])});
    }

    return cards;
}

bool MysqlSession::insertCard(Card const& c) {
    try {
        sql("INSERT INTO card(card_no,xuid,slot_index) VALUES(" + quote(c.no) + "," + quote(c.xuid) + "," + num(c.slot)
            + ")");

        return true;
    } catch (Failure const& e) {
        if (e.code == "DUPLICATE") {
            return false;
        }

        throw;
    }
}

void MysqlSession::saveCard(Card const& c) {
    sql("UPDATE card SET balance_base=" + num(c.balance) + ",version=" + num(c.version) + ",status=" + quote(c.status)
        + " WHERE card_no=" + quote(c.no) + " AND version=" + num(c.version - 1));
    require(pool.client()->affectedRows(connection) == 1, "CONFLICT", "银行卡版本冲突，请重试");
}

std::vector<Lot> MysqlSession::lots(std::string const& no) {
    std::vector<Lot> lots;
    for (auto const& r :
         sql("SELECT id,card_no,base_amount,remain_base,origin_server_id,created_at FROM card_lot WHERE card_no="
             + quote(no) + " AND remain_base>0 ORDER BY created_at,id" + forUpdate())) {
        lots.push_back({r[0], r[1], r[4], integer(r[2]), integer(r[3]), integer(r[5])});
    }

    return lots;
}

void MysqlSession::insertLot(Lot const& l) {
    sql("INSERT INTO card_lot VALUES(" + quote(l.id) + "," + quote(l.card) + "," + num(l.original) + "," + num(l.remain)
        + "," + quote(l.origin) + "," + num(l.created) + ")");
}

void MysqlSession::consumeLot(std::string const& id, Amount amount) {
    sql("UPDATE card_lot SET remain_base=remain_base-" + num(amount) + " WHERE id=" + quote(id)
        + " AND remain_base>=" + num(amount));
    require(pool.client()->affectedRows(connection) == 1, "CONFLICT", "资金批次已改变");
}

void MysqlSession::appendLedger(LedgerEntry const& l) {
    sql("INSERT INTO ledger VALUES(" + quote(l.id) + "," + quote(l.xuid) + "," + quote(l.card) + "," + quote(l.kind)
        + "," + num(l.delta) + "," + num(l.fee) + "," + num(l.balance) + "," + num(l.rate) + "," + num(l.local) + ","
        + quote(l.server) + "," + quote(l.peer) + "," + quote(l.intent) + "," + num(l.created) + ")");
}

std::vector<LedgerEntry> MysqlSession::history(std::string const& xuid, int count) {
    std::vector<LedgerEntry> entries;
    for (auto const& r :
         sql("SELECT id,xuid,card_no,kind,delta_base,fee_base,balance_after,rate,local_amount,server_id,peer_xuid,"
             "intent_id,created_at FROM ledger WHERE xuid="
             + quote(xuid) + " ORDER BY created_at DESC,id DESC LIMIT " + num(std::clamp(count, 1, 100)))) {
        LedgerEntry entry;
        entry.id      = r[0];
        entry.xuid    = r[1];
        entry.card    = r[2];
        entry.kind    = r[3];
        entry.delta   = integer(r[4]);
        entry.fee     = integer(r[5]);
        entry.balance = integer(r[6]);
        entry.rate    = integer(r[7]);
        entry.local   = integer(r[8]);
        entry.server  = r[9];
        entry.peer    = r[10];
        entry.intent  = r[11];
        entry.created = integer(r[12]);
        entries.push_back(entry);
    }

    return entries;
}

Amount MysqlSession::daily(std::string const& xuid, Direction dir, Time since) {
    auto rows =
        sql("SELECT COALESCE(SUM(base_amount),0) FROM intent WHERE xuid=" + quote(xuid) + " AND dir=" + quote(name(dir))
            + " AND created_at>=" + num(since) + " AND state<>'ROLLED_BACK'");

    return integer(rows[0][0]);
}

void MysqlSession::insertIntent(Intent const& i) {
    require(i.requestKey.size() <= 128, "KEY", "请求标识过长");

    Json slices = Json::array();
    for (auto const& s : i.slices) {
        slices.push_back({
            {"lot",     s.lot    },
            {"origin",  s.origin },
            {"amount",  s.amount },
            {"created", s.created}
        });
    }

    // request_key 为 NULL 时不占用唯一索引；撞索引即第一笔已生效，由调用方原样返回。
    sql(std::string("INSERT INTO intent(") + intentColumns + ") VALUES(" + quote(i.id) + "," + quote(i.xuid) + ","
        + quote(name(i.dir)) + "," + quote(i.card) + "," + num(i.base) + "," + num(i.fee) + "," + num(i.local) + ","
        + num(i.rate) + "," + quote(i.server) + "," + quote(name(i.state)) + ",0," + quote(i.error) + ","
        + (i.requestKey.empty() ? "NULL" : quote(i.requestKey)) + "," + quote(i.peer) + "," + quote(slices.dump()) + ","
        + num(i.created) + "," + num(i.updated) + ")");
}

std::optional<Intent> MysqlSession::intent(std::string const& id) {
    auto rows = sql(std::string("SELECT ") + intentColumns + " FROM intent WHERE intent_id=" + quote(id) + forUpdate());
    if (rows.empty()) {
        return {};
    }

    return parseIntent(rows[0]);
}

bool MysqlSession::cas(std::string const& id, State from, State to, Time time, std::string const& error) {
    sql("UPDATE intent SET state=" + quote(name(to)) + ",updated_at=" + num(time) + ",attempts=attempts+1,error="
        + quote(error) + " WHERE intent_id=" + quote(id) + " AND state=" + quote(name(from)));

    return pool.client()->affectedRows(connection) == 1;
}

bool MysqlSession::unsettled(std::string const& xuid) {
    return !sql("SELECT intent_id FROM intent WHERE xuid=" + quote(xuid)
                + " AND state NOT IN ('SETTLED','ROLLED_BACK') LIMIT 1")
                .empty();
}

std::vector<Intent> MysqlSession::pending(Time before) {
    std::vector<Intent> intents;
    for (auto const& r :
         sql(std::string("SELECT ") + intentColumns
             + " FROM intent WHERE state IN ('PENDING_DB','PENDING_LEGACY') AND updated_at<=" + num(before)
             + " ORDER BY updated_at LIMIT 200")) {
        intents.push_back(parseIntent(r));
    }

    return intents;
}

std::vector<Node> MysqlSession::nodes() {
    std::vector<Node> nodes;
    for (auto const& r : sql("SELECT server_id,server_name,currency,plugin_ver,status,last_heartbeat FROM node")) {
        nodes.push_back({r[0], r[1], r[2], r[3], r[4], integer(r[5])});
    }

    return nodes;
}

bool MysqlSession::registerNode(Node const& n) {
    sql("INSERT IGNORE INTO node VALUES(" + quote(n.id) + "," + quote(n.name) + "," + quote(n.currency) + ","
        + quote(n.version) + "," + num(n.heartbeat) + ",'ONLINE')");

    // 同名不一致说明 server_id 被另一套身份占用，只能降级只读。
    auto rows = sql("SELECT server_name,currency FROM node WHERE server_id=" + quote(n.id));

    return !rows.empty() && rows[0][0] == n.name && rows[0][1] == n.currency;
}

void MysqlSession::heartbeat(Node const& n) {
    sql("UPDATE node SET last_heartbeat=" + num(n.heartbeat) + ",plugin_ver=" + quote(n.version)
        + ",status=" + quote(n.status) + " WHERE server_id=" + quote(n.id) + " AND server_name=" + quote(n.name)
        + " AND currency=" + quote(n.currency));
}

std::vector<Rate> MysqlSession::rates(Time now) {
    std::vector<Rate> rates;
    for (auto const& r :
         sql("SELECT server_id,rate_bigint,effective_at,set_by FROM (SELECT *,ROW_NUMBER() OVER(PARTITION BY "
             "server_id ORDER BY effective_at DESC,id DESC) rn FROM rate WHERE effective_at<="
             + num(now) + ") r WHERE rn=1 ORDER BY server_id")) {
        rates.push_back({r[0], r[3], integer(r[1]), integer(r[2])});
    }

    return rates;
}

void MysqlSession::appendRate(Rate const& r) {
    sql("INSERT INTO rate(server_id,rate_bigint,effective_at,set_by) VALUES(" + quote(r.server) + "," + num(r.value)
        + "," + num(r.effective) + "," + quote(r.by) + ")");
}

HotConfig MysqlSession::config() {
    auto rows = sql("SELECT v FROM config WHERE k='bank'");

    return rows.empty() ? HotConfig{} : bootstrap::hotConfig(Json::parse(rows[0][0]));
}

void MysqlSession::putConfig(HotConfig const& c, std::string const& by) {
    auto j = bootstrap::hotJson(c);
    bootstrap::hotConfig(j);

    sql("INSERT INTO config(k,v,updated_by) VALUES('bank'," + quote(j.dump()) + "," + quote(by)
        + ") ON DUPLICATE KEY UPDATE v=VALUES(v),version=version+1,updated_by=VALUES(updated_by)");
}

void MysqlSession::appendAudit(AuditRecord const& a) {
    sql("INSERT INTO audit_legacy VALUES(" + quote(a.id) + "," + quote(a.xuid) + "," + quote(a.event) + ","
        + (a.delta ? num(*a.delta) : "NULL") + "," + num(a.balance) + "," + quote(a.server) + "," + num(a.created)
        + ")");
}

Amount MysqlSession::sequence(std::string const& key) {
    sql("INSERT INTO card_gen_seq(k,v) VALUES(" + quote(key) + ",1) ON DUPLICATE KEY UPDATE v=v+1");
    auto rows = sql("SELECT v FROM card_gen_seq WHERE k=" + quote(key) + " FOR UPDATE");

    return integer(rows[0][0]);
}

void MysqlSession::migrate(std::filesystem::path const& path) {
    auto lock = sql("SELECT GET_LOCK('rbank:migration',10)");
    require(!lock.empty() && lock[0][0] == "1", "BUSY", "迁移锁繁忙");

    try {
        std::ifstream in(path);
        require(in.good(), "MIGRATION", "迁移脚本缺失: " + path.string());
        std::stringstream text;
        text << in.rdbuf();
        std::string statement;
        while (std::getline(text, statement, ';')) {
            if (statement.find_first_not_of(" \r\n\t") != std::string::npos) {
                sql(statement);
            }
        }

        sql("SELECT RELEASE_LOCK('rbank:migration')");
    } catch (...) {
        try {
            sql("SELECT RELEASE_LOCK('rbank:migration')");
        } catch (...) {
            broken = true;
        }

        throw;
    }
}

Database::Database(DatabaseConfig c) : cfg(std::move(c)), api(std::make_shared<Api>(cfg.library)) {}

Database::~Database() {
    for (auto connection : idle) {
        api->close(connection);
    }
}

void* Database::acquire() {
    // 每个工作线程持有自己的客户端库线程上下文，直到线程结束。
    thread_local ThreadContext context(client());

    std::unique_lock lock(mutex);
    require(std::chrono::steady_clock::now() >= circuitUntil, "UNAVAILABLE", "银行维护中");
    auto limit = static_cast<size_t>(std::max(cfg.pool, 1));
    if (!available.wait_for(lock, std::chrono::seconds(2), [this, limit] { return !idle.empty() || count < limit; })) {
        throw Failure("BUSY", "银行繁忙");
    }

    if (!idle.empty()) {
        auto connection = idle.back();
        idle.pop_back();

        return connection;
    }

    ++count;
    lock.unlock();

    auto connection = api->init(nullptr);
    if (connection) {
        api->options(connection, 0, &cfg.timeout);
        api->options(connection, 11, &cfg.timeout);
        api->options(connection, 12, &cfg.timeout);

        // Connector/C 的官方枚举：SSL_ENFORCE=38、SSL_VERIFY_SERVER_CERT=21、SSL_CA=27。
        unsigned char enforce = 1;
        unsigned char verify  = cfg.verifyCertificate ? 1 : 0;
        api->options(connection, 38, &enforce);
        api->options(connection, 21, &verify);
        if (!cfg.caFile.empty()) {
            api->options(connection, 27, cfg.caFile.c_str());
        }

        // 绝不开启自动重连：重连会静默丢掉 GET_LOCK 所有权。
        if (api->connect(
                connection,
                cfg.host.c_str(),
                cfg.user.c_str(),
                cfg.password.c_str(),
                cfg.database.c_str(),
                cfg.port,
                nullptr,
                0
            )) {
            std::string setup = "SET NAMES utf8mb4";
            auto        run   = [&](std::string const& statement) {
                return api->query(connection, statement.c_str(), static_cast<unsigned long>(statement.size())) == 0;
            };
            if (run(setup) && run("SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED")) {
                return connection;
            }
        }
        api->close(connection);
    }

    // 连接失败：归还名额并熔断 5 秒，让银行域操作快速失败。
    circuitUntil = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    --count;
    available.notify_one();

    throw Failure("UNAVAILABLE", "银行数据库不可用");
}

void Database::release(void* connection, bool broken) {
    std::lock_guard lock(mutex);
    if (connection == nullptr) {
        --count;
        available.notify_one();

        return;
    }
    if (broken) {
        api->close(connection);
        --count;
    } else {
        idle.push_back(connection);
    }
    available.notify_one();
}

std::unique_ptr<Session> Database::open(std::vector<std::string> xuids) {
    return std::make_unique<MysqlSession>(*this, std::move(xuids));
}

void Database::migrate(std::filesystem::path const& script) {
    MysqlSession session(*this, {});
    session.migrate(script);
}

} // namespace rbank::infra
