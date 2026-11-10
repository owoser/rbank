#pragma once

#include "bootstrap/config/Config.h"

#include <memory>
#include <optional>
#include <vector>

#include "ll/api/event/ListenerBase.h"
#include "ll/api/mod/NativeMod.h"

namespace rbank {
class Application;
}

namespace rbank::bootstrap {

/// 插件生命周期：读启动配置、装配组合根、挂接事件，卸载时按相反顺序退订。
class Plugin {
    std::optional<StartupConfig>        config;
    std::shared_ptr<Application>        app;
    std::vector<ll::event::ListenerPtr> listeners;
    bool                                subscribed = false;

    void start();

public:
    static Plugin& instance();

    bool load();
    bool enable();
    bool disable();
    bool unload();

    ll::mod::NativeMod& self() const { return *ll::mod::NativeMod::current(); }
};

} // namespace rbank::bootstrap
