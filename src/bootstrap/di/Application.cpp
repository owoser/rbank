#include "bootstrap/di/Application.h"

#include "adapter/common/Presentation.h"
#include "adapter/http/HttpServer.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/thread/ServerThreadExecutor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

namespace rbank {

using namespace core;

Application::Application(
    bootstrap::StartupConfig         c,
    std::filesystem::path const&     migration,
    std::function<void(std::string)> logger
)
: cfg(std::move(c)),
  log(std::move(logger)),
  database(
      {cfg.database.host,
       cfg.database.port,
       cfg.database.user,
       cfg.database.password,
       cfg.database.database,
       cfg.database.library,
       cfg.database.caFile,
       cfg.database.pool,
       cfg.database.timeout,
       cfg.database.verifyCertificate}
  ),
  generator(cfg.card),
  money(active),
  bank(database, money, clock, ids, generator, cfg.node),
  workers(static_cast<unsigned>(cfg.workers), static_cast<size_t>(cfg.queue)) {
    try {
        // 迁移与首次注册都在工作线程完成，不阻塞服务端启动。
        maintenanceRunning = true;
        workers.submit([this, migration] {
            try {
                database.migrate(migration);
                maintain();
            } catch (std::exception const& e) {
                log(e.what());
            }
            maintenanceRunning = false;
        });

        if (cfg.http.enabled) {
            http = std::make_unique<adapter::HttpServer>(*this, cfg.http);
        }

        timer = std::jthread([this, migration](std::stop_token token) {
            std::mutex                  waitMutex;
            std::condition_variable_any cv;
            std::unique_lock            lock(waitMutex);
            unsigned                    tick = 0;
            while (!token.stop_requested()) {
                cv.wait_for(lock, token, std::chrono::seconds(proto::Protocol::ratePollSeconds), [] { return false; });
                if (token.stop_requested()) {
                    break;
                }
                if (maintenanceRunning.exchange(true)) {
                    continue;
                }

                bool full = ++tick % proto::Protocol::heartbeatTicks == 0;
                if (!workers.submit([this, migration, full] {
                        try {
                            if (full) {
                                if (!initialized.load()) {
                                    database.migrate(migration);
                                }
                                maintain();
                            } else if (initialized.load()) {
                                pollRates();
                            }
                        } catch (std::exception const& e) {
                            if (++failures % 30 == 1) {
                                log(e.what());
                            }
                        }
                        maintenanceRunning = false;
                    })) {
                    maintenanceRunning = false;
                }
            }
        });
    } catch (...) {
        stop();
        throw;
    }
}

Application::~Application() { stop(); }

void Application::stop() {
    if (!active->exchange(false)) {
        return;
    }

    timer.request_stop();
    if (timer.joinable()) {
        timer.join();
    }

    if (http) {
        http->stop();
    }
    workers.stop();
}

bool Application::submit(std::function<Result(core::Bank&)> task, Reply reply) {
    auto running = active;
    if (!active->load()) {
        return false;
    }

    return workers.submit([this, running, task = std::move(task), reply = std::move(reply)] {
        auto result = initialized.load() ? guarded([&] { return task(bank); })
                                         : Result::fail("UNAVAILABLE", "Bank is under maintenance, try again later");

        // 表单与消息只能在主线程回调，且只回传值，不跨线程保存 Player 引用。
        ll::thread::ServerThreadExecutor::getDefault().execute([running, result = std::move(result), reply] {
            if (running->load()) {
                reply(result);
            }
        });
    });
}

void Application::maintain() {
    auto s         = database.open();
    auto node      = cfg.node;
    node.heartbeat = clock.now();

    if (!registered) {
        registered = s->registerNode(node);
        if (!registered) {
            log("server_id does not match the registered name or currency; bank downgraded to read-only");
            initialized = true;

            return;
        }
    }
    s->heartbeat(node);

    std::deque<AuditRecord> batch;
    {
        std::lock_guard lock(auditMutex);
        batch.swap(audits);
    }
    if (!batch.empty()) {
        try {
            Transaction tx(*s);
            for (auto const& record : batch) {
                s->appendAudit(record);
            }
            tx.commit();
        } catch (...) {
            // 审计是只读旁路：写库失败只丢弃，绝不影响原版经济。
            if (++failures % 10 == 1) {
                log("Audit batch write failed and was dropped");
            }
        }
    }

    s.reset();
    initialized = true;

    pollRates();
    bank.recover(proto::Protocol::recoveryGraceSeconds, proto::Protocol::ownerOfflineSeconds);
    failures = 0;
}

void Application::pollRates() {
    auto s       = database.open();
    auto current = s->rates(clock.now());
    s.reset();

    for (auto const& rate : rates.changes(current)) {
        auto running = active;
        auto server  = rate.server;
        auto value   = rate.value;
        ll::thread::ServerThreadExecutor::getDefault().execute([running, server, value] {
            if (!running->load()) {
                return;
            }

            auto level = ll::service::getLevel();
            if (level) {
                level->forEachPlayer([&](Player& player) {
                    player.sendMessage(
                        adapter::translate(
                            {
                                "[rbank] Rate for {0} is now {1} / base",
                                {server, std::to_string(value)}
                    },
                            player.getLocaleCode()
                        )
                    );

                    return true;
                });
            }
        });
    }
}

void Application::audit(
    infra::LegacyEvent event,
    std::string const& from,
    std::string const& to,
    Amount             value
) noexcept {
    try {
        if (!active->load() || !initialized.load()) {
            return;
        }

        std::lock_guard lock(auditMutex);
        auto            push = [&](std::string const& xuid, std::string const& kind, std::optional<Amount> delta) {
            if (xuid.empty()) {
                return;
            }

            auto balance = money.balance(xuid);
            if (balance < 0) {
                return;
            }
            if (!delta) {
                if (auto it = lastBalances.find(xuid); it != lastBalances.end()) {
                    delta = balance - it->second;
                }
            }

            lastBalances[xuid] = balance;
            if (audits.size() < 1024) {
                audits.push_back({ids.next(), xuid, kind, cfg.node.id, delta, balance, clock.now()});
            }
        };

        if (event == infra::LegacyAdd) {
            push(to, "ADD", value);
        } else if (event == infra::LegacyReduce) {
            push(to, "REDUCE", -value);
        } else if (event == infra::LegacySet) {
            push(to, "SET", {});
        } else {
            push(from, "TRANS_OUT", -value);
            push(to, "TRANS_IN", {});
        }
    } catch (...) {
        // 任何异常都不得回传到 LegacyMoney 的调用栈。
    }
}

void Application::join(std::string const& xuid, Reply reply) {
    submit(
        [this, xuid](core::Bank& b) {
            if (!database.open()->config().joinForm) {
                return Result::fail("SKIP");
            }

            auto cards   = b.list(xuid);
            auto history = b.history(xuid, 3);

            return Result::success({
                {"cards",   cards.ok() ? std::move(cards.data) : core::Json::array()},
                {"history", std::move(history.data)                                 }
            });
        },
        std::move(reply)
    );
}

} // namespace rbank
