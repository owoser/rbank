#include "infra/legacymoney/LegacyMoney.h"

#include "ll/api/service/Bedrock.h"
#include "ll/api/thread/ServerThreadExecutor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <future>

namespace rbank::infra {

using namespace core;

LegacyMoney::LegacyMoney(std::shared_ptr<std::atomic_bool> a) : active(std::move(a)) {
    auto module = GetModuleHandleW(L"LegacyMoney.dll");
    require(module != nullptr, "LEGACY", "LegacyMoney 尚未加载");

    read     = reinterpret_cast<Read>(GetProcAddress(module, "LLMoney_Get"));
    addFn    = reinterpret_cast<Write>(GetProcAddress(module, "LLMoney_Add"));
    reduceFn = reinterpret_cast<Write>(GetProcAddress(module, "LLMoney_Reduce"));
    listen   = reinterpret_cast<Listen>(GetProcAddress(module, "LLMoney_ListenAfterEvent"));
    require(read && addFn && reduceFn && listen, "LEGACY", "LegacyMoney 接口版本不兼容");
}

namespace {

bool online(std::string const& xuid) {
    auto level = ll::service::getLevel();
    if (!level) {
        return false;
    }

    bool found = false;
    level->forEachPlayer([&](Player& player) {
        if (player.getXuid() == xuid) {
            found = true;
        }

        return !found;
    });

    return found;
}

/// 主线程往返：超时或插件卸载后返回 nullopt，调用方据此判定结果为 Unknown。
template <class T, class Fn>
std::optional<T> onMain(std::shared_ptr<std::atomic_bool> const& active, Fn fn) {
    struct Work {
        std::atomic_int state = 0;
        std::promise<T> promise;
    };

    auto work   = std::make_shared<Work>();
    auto result = work->promise.get_future();
    ll::thread::ServerThreadExecutor::getDefault().execute([active, work, fn = std::move(fn)] {
        int queued = 0;
        if (!active->load() || !work->state.compare_exchange_strong(queued, 1)) {
            return;
        }

        try {
            work->promise.set_value(fn());
        } catch (...) {
            work->promise.set_exception(std::current_exception());
        }
    });

    if (result.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        int queued = 0;
        work->state.compare_exchange_strong(queued, 2);

        return {};
    }

    try {
        return result.get();
    } catch (...) {
        return {};
    }
}

} // namespace

std::optional<Amount> LegacyMoney::get(std::string const& xuid) {
    auto fn     = read;
    auto result = onMain<Amount>(active, [fn, xuid] { return online(xuid) ? fn(xuid) : -1; });
    if (!result || *result < 0) {
        return {};
    }

    return result;
}

MoneyOutcome LegacyMoney::invoke(Write fn, std::string const& xuid, Amount value) {
    require(value > 0, "AMOUNT", "金额必须为正整数");

    auto result = onMain<bool>(active, [fn, xuid, value] { return online(xuid) && fn(xuid, value); });
    if (!result) {
        return MoneyOutcome::Unknown;
    }

    return *result ? MoneyOutcome::Success : MoneyOutcome::Failed;
}

} // namespace rbank::infra
