#pragma once

#include "core/port/Ports.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rbank::infra {

struct DatabaseConfig {
    std::string host = "127.0.0.1";
    unsigned    port = 3306;
    std::string user, password, database = "rbank", library = "libmariadb.dll", caFile;
    int         pool              = 4;
    unsigned    timeout           = 3;
    bool        verifyCertificate = true;
};

/// 直连 MySQL 的连接池。每线程初始化一次客户端库线程上下文；
/// 连接一旦断开即销毁，绝不重新连接（重连会丢失 GET_LOCK 所有权）。
class Database final : public core::UnitOfWork {
public:
    struct Api;

private:
    DatabaseConfig       cfg;
    std::shared_ptr<Api> api;

    std::mutex                            mutex;
    std::condition_variable               available;
    std::vector<void*>                    idle;
    size_t                                count = 0;
    std::chrono::steady_clock::time_point circuitUntil;

public:
    void* acquire();
    void  release(void* connection, bool broken);

    std::shared_ptr<Api> const& client() const { return api; }

    explicit Database(DatabaseConfig);
    ~Database() override;

    std::unique_ptr<core::Session> open(std::vector<std::string> xuids = {}) override;

    /// 建表迁移：在独占连接上持迁移锁执行 schema 脚本。
    void migrate(std::filesystem::path const& script);
};

} // namespace rbank::infra
