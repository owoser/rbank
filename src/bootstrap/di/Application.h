#pragma once

#include "bootstrap/config/Config.h"
#include "core/service/Bank.h"
#include "infra/cardgen/CardGenerator.h"
#include "infra/legacymoney/LegacyMoney.h"
#include "infra/mysql/Database.h"
#include "infra/runtime/Clock.h"
#include "infra/runtime/WorkerPool.h"
#include "proto/Protocol.h"

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <thread>

namespace rbank {

namespace adapter {
class HttpServer;
}

/// 组合根：装配端口实现、驱动后台维护，并把用例调度到工作线程与主线程。
class Application {
    using Reply = std::function<void(core::Result)>;

    bootstrap::StartupConfig          cfg;
    std::function<void(std::string)>  log;
    std::shared_ptr<std::atomic_bool> active = std::make_shared<std::atomic_bool>(true);

    infra::Database      database;
    infra::SystemClock   clock;
    infra::RandomId      ids;
    infra::CardGenerator generator;
    infra::LegacyMoney   money;
    core::Bank           bank;
    infra::WorkerPool    workers;
    std::jthread         timer;

    std::atomic_bool                     initialized        = false;
    std::atomic_bool                     maintenanceRunning = false;
    std::unique_ptr<adapter::HttpServer> http;

    std::mutex                          auditMutex;
    std::deque<core::AuditRecord>       audits;
    std::map<std::string, core::Amount> lastBalances;
    proto::RateBroadcast                rates;

    bool     registered = false;
    unsigned failures   = 0;

    void maintain();
    void pollRates();

public:
    Application(bootstrap::StartupConfig, std::filesystem::path const& migration, std::function<void(std::string)>);
    ~Application();

    std::string requestKey() { return ids.next(); }
    bool        alive() const { return active->load(); }

    bool submit(std::function<core::Result(core::Bank&)> task, Reply reply);
    void audit(infra::LegacyEvent, std::string const&, std::string const&, core::Amount) noexcept;
    void join(std::string const& xuid, Reply);
    void stop();

    bootstrap::StartupConfig const& config() const { return cfg; }
    core::UnitOfWork&               store() { return database; }
    core::Bank&                     services() { return bank; }
};

} // namespace rbank
