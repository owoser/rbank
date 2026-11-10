#include "adapter/http/HttpServer.h"

#include "bootstrap/config/Config.h"
#include "bootstrap/di/Application.h"

#include <httplib.h>

#include <thread>

namespace rbank::adapter {

using namespace core;

namespace {

Json envelope(Result const& r) {
    return {
        {"code",    r.code   },
        {"message", r.message},
        {"data",    r.data   }
    };
}

Result nodes(Application& app) {
    auto s    = app.store().open();
    Json data = Json::array();
    for (auto const& n : s->nodes()) {
        data.push_back({
            {"server_id", n.id       },
            {"name",      n.name     },
            {"currency",  n.currency },
            {"status",    n.status   },
            {"heartbeat", n.heartbeat}
        });
    }

    return Result::success(std::move(data));
}

Result readConfig(Application& app) {
    auto s = app.store().open();

    return Result::success(bootstrap::hotJson(s->config()));
}

Result writeConfig(Application& app, std::string const& body) {
    auto        hot = bootstrap::hotConfig(Json::parse(body));
    auto        s   = app.store().open();
    Transaction tx(*s);
    s->putConfig(hot, "http-admin");
    tx.commit();

    return Result::success(bootstrap::hotJson(hot));
}

Result intentInfo(Application& app, std::string const& id) {
    auto found = app.store().open()->intent(id);
    require(found.has_value(), "INTENT", "单据不存在");

    return Result::success({
        {"intent_id",    found->id         },
        {"xuid",         found->xuid       },
        {"card_no",      found->card       },
        {"direction",    name(found->dir)  },
        {"state",        name(found->state)},
        {"base_amount",  found->base       },
        {"fee_base",     found->fee        },
        {"local_amount", found->local      },
        {"rate",         found->rate       },
        {"server_id",    found->server     },
        {"attempts",     found->attempts   },
        {"error",        found->error      }
    });
}

} // namespace

struct HttpServer::Impl {
    Application&          app;
    bootstrap::HttpConfig config;
    httplib::Server       server;
    std::thread           thread;

    bool authorized(httplib::Request const& request) const {
        // 常量时间比较，避免通过响应时间枚举令牌。
        auto   actual   = request.get_header_value("Authorization");
        auto   expected = "Bearer " + config.token;
        size_t diff     = actual.size() ^ expected.size();
        for (size_t i = 0; i < expected.size(); ++i) {
            diff |= static_cast<unsigned char>(expected[i])
                  ^ (i < actual.size() ? static_cast<unsigned char>(actual[i]) : 0);
        }

        return diff == 0;
    }

    template <class Fn>
    auto guard(Fn fn) {
        return [this, fn](httplib::Request const& request, httplib::Response& response) {
            response.set_header("Cache-Control", "no-store");
            response.set_header("X-Content-Type-Options", "nosniff");

            if (!authorized(request)) {
                response.status = 401;
                response.set_content(
                    envelope(Result::fail("AUTH", "需要管理令牌")).dump(),
                    "application/json; charset=utf-8"
                );

                return;
            }

            Result result;
            try {
                result = fn(request);
            } catch (Json::exception const&) {
                result = Result::fail("INPUT", "JSON 参数无效");
            } catch (Failure const& e) {
                result = Result::fail(e.code, e.what());
            } catch (...) {
                result = Result::fail("UNAVAILABLE", "银行维护中");
            }

            response.status = result.ok() ? 200 : result.code == "UNAVAILABLE" ? 503 : 400;
            response.set_content(envelope(result).dump(), "application/json; charset=utf-8");
        };
    }

    template <class Fn>
    void get(std::string const& pattern, Fn fn) {
        server.Get(pattern, guard(std::move(fn)));
    }

    template <class Fn>
    void post(std::string const& pattern, Fn fn) {
        server.Post(pattern, guard(std::move(fn)));
    }

    Impl(Application& a, bootstrap::HttpConfig c) : app(a), config(std::move(c)) {
        server.new_task_queue = [] { return new httplib::ThreadPool(2, 32); };
        server.set_payload_max_length(16384);
        server.set_read_timeout(3);
        server.set_write_timeout(3);

        get(R"(/v1/cards/([0-9]{1,20}))", [this](auto const& r) { return app.services().list(r.matches[1].str()); });
        get(R"(/v1/ledger/([0-9]{1,20}))",
            [this](auto const& r) { return app.services().history(r.matches[1].str(), 100); });
        get("/v1/nodes", [this](auto const&) { return nodes(app); });
        get("/v1/rates", [this](auto const&) { return app.services().rates(); });
        get("/v1/config", [this](auto const&) { return readConfig(app); });
        get(R"(/v1/intents/([a-f0-9]{32}))", [this](auto const& r) { return intentInfo(app, r.matches[1].str()); });

        server.Put("/v1/config", guard([this](auto const& r) { return writeConfig(app, r.body); }));
        post(R"(/v1/cards/([A-Za-z0-9]{4,64})/freeze)", [this](auto const& r) {
            return app.services().freeze(r.matches[1].str(), Json::parse(r.body).at("frozen").get<bool>());
        });
        post("/v1/rates", [this](auto const& r) {
            auto j = Json::parse(r.body);
            require(j.at("rate").is_number_integer(), "INPUT", "汇率必须为整数");

            return app.services()
                .setRate(j.at("server_id").get<std::string>(), j.at("rate").get<Amount>(), "http-admin");
        });
        post(R"(/v1/intents/([a-f0-9]{32})/resolve)", [this](auto const& r) {
            auto applied = Json::parse(r.body).at("legacy_applied").get<bool>();

            return app.services().resolve(r.matches[1].str(), applied, "http-admin");
        });

        if (!server.bind_to_port(config.host, config.port)) {
            throw std::runtime_error("HTTP 监听失败: " + config.host + ":" + std::to_string(config.port));
        }
        thread = std::thread([this] { server.listen_after_bind(); });
    }

    void stop() {
        server.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }

    ~Impl() { stop(); }
};

HttpServer::HttpServer(Application& app, bootstrap::HttpConfig cfg)
: impl(std::make_unique<Impl>(app, std::move(cfg))) {}

HttpServer::~HttpServer() = default;

void HttpServer::stop() { impl->stop(); }

} // namespace rbank::adapter
