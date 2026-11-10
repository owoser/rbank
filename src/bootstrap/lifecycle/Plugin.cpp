#include "bootstrap/lifecycle/Plugin.h"

#include "adapter/command/Commands.h"
#include "bootstrap/di/Application.h"
#include "infra/legacymoney/LegacyMoney.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/command/ServerCommandRegisterEvent.h"
#include "ll/api/event/player/PlayerJoinEvent.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/Bedrock.h"

#include <atomic>

namespace rbank::bootstrap {

namespace {

/// LegacyMoney 的 after 回调没有退订接口，因此用弱引用桥接；插件卸载后回调自然失效。
std::atomic<std::weak_ptr<Application>> auditBridge;

bool onLegacyEvent(infra::LegacyEvent event, std::string from, std::string to, long long value) noexcept {
    try {
        if (auto app = auditBridge.load().lock()) {
            app->audit(event, from, to, value);
        }
    } catch (...) {}

    return true;
}

} // namespace

Plugin& Plugin::instance() {
    static Plugin plugin;

    return plugin;
}

void Plugin::start() {
    auto self = ll::mod::NativeMod::current();
    app       = std::make_shared<Application>(
        *config,
        self->getModDir() / "schema/migrations/001_bank.sql",
        [self](std::string const& message) { self->getLogger().warn("rbank: {}", message); }
    );
    auditBridge.store(app);

    if (!subscribed) {
        // after 订阅只需一次；用临时探针实例取得函数指针即可，不保留句柄。
        infra::LegacyMoney probe(std::make_shared<std::atomic_bool>(true));
        probe.subscribe(onLegacyEvent);
        subscribed = true;
    }
}

bool Plugin::load() {
    try {
        config    = StartupConfig{};
        auto path = self().getConfigDir() / "config.json";
        if (!loadConfig(path, *config)) {
            self().getLogger().error("rbank: 已生成 {}，请填写数据库与节点身份后重启", path.string());

            return false;
        }
        if (std::filesystem::path(config->database.library).is_relative()) {
            config->database.library =
                std::filesystem::absolute(self().getModDir() / config->database.library).string();
        }
        start();

        // setupStandaloneServer 在 onEnable 之前发布该事件，因此这里同步挂接。
        std::weak_ptr weak = app;
        listeners.push_back(
            ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerCommandRegisterEvent>([weak](auto&) {
                if (auto app = weak.lock()) {
                    adapter::registerCommands(app);
                }
            })
        );

        return true;
    } catch (std::exception const& e) {
        self().getLogger().error("rbank: {}", e.what());
        disable();

        return false;
    }
}

bool Plugin::enable() {
    try {
        if (!app) {
            start();
            if (ll::service::getCommandRegistry(false)) {
                adapter::registerCommands(app);
            }
        }

        std::weak_ptr weak = app;
        listeners.push_back(
            ll::event::EventBus::getInstance().emplaceListener<ll::event::PlayerJoinEvent>([weak](auto& event) {
                if (auto app = weak.lock(); app && app->alive()) {
                    adapter::onJoin(app, event.self());
                }
            })
        );

        self().getLogger().info("rbank 已启用，节点 {}，数据库初始化在工作线程执行", config->node.id);

        return true;
    } catch (std::exception const& e) {
        self().getLogger().error("rbank: {}", e.what());
        disable();

        return false;
    }
}

bool Plugin::disable() {
    auditBridge.store({});

    auto& bus = ll::event::EventBus::getInstance();
    for (auto const& listener : listeners) {
        if (listener) {
            bus.removeListener(listener);
        }
    }
    listeners.clear();

    if (app) {
        app->stop();
        app.reset();
    }

    return true;
}

bool Plugin::unload() {
    if (subscribed) {
        self().getLogger().warn("LegacyMoney 不支持退订 after 回调，rbank 需重启服务端才能卸载");

        return false;
    }

    return true;
}

} // namespace rbank::bootstrap

LL_REGISTER_MOD(rbank::bootstrap::Plugin, rbank::bootstrap::Plugin::instance());
