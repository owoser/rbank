#include "adapter/command/Commands.h"

#include "adapter/command/CommandArguments.h"
#include "adapter/common/Presentation.h"
#include "bootstrap/di/Application.h"
#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/mod/NativeMod.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/service/PlayerInfo.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

namespace rbank::adapter {

using namespace core;

template <class T>
struct CardParam {
    T card;
};

template <class T>
struct AmountParam {
    T               card;
    IntegerArgument amount;
};

template <class T, class R>
struct PayParam {
    T               card;
    R               recipient;
    IntegerArgument amount;
};

struct TargetParam {
    IntegerArgument xuid;
};

template <class T>
struct RateParam {
    T               server;
    IntegerArgument amount;
};

struct SlotParam {
    int count = 1;
};


namespace {

Player* player(CommandOrigin const& origin) {
    auto entity = origin.getEntity();

    return entity && entity->isPlayer() ? static_cast<Player*>(entity) : nullptr;
}

bool operator_(CommandOrigin const& origin, CommandOutput& output) {
    if (origin.getPermissionsLevel() < CommandPermissionLevel::GameDirectors) {
        output.error(translate("Operator permission is required", origin.getLocaleCode()));

        return false;
    }

    return true;
}

/// 结果回到主线程后再发送：玩家看消息，控制台看日志，不跨线程持有引用。
void dispatch(
    std::weak_ptr<Application>   weak,
    CommandOrigin const&         origin,
    CommandOutput&               output,
    std::function<Result(Bank&)> task
) {
    auto app = weak.lock();
    if (!app || !app->alive()) {
        output.error(translate("Bank is under maintenance", origin.getLocaleCode()));
        return;
    }

    auto user = player(origin);
    auto xuid = user ? user->getXuid() : std::string{};
    auto ok   = app->submit(std::move(task), [xuid](Result result) {
        if (xuid.empty()) {
            ll::mod::NativeMod::current()->getLogger().info("{}", render(result));

            return;
        }

        playerByXuid(xuid, [&](Player& p) { p.sendMessage(render(result, p.getLocaleCode())); });
    });
    if (!ok) {
        output.error(translate("Bank is busy, try again later", origin.getLocaleCode()));
    }
}

/// 字符串与整数在 BDS 语法中是两条分支：卡号、收款人、服务器 ID 两种都注册。
template <class Fn>
void forBoth(Fn&& fn) {
    fn.template operator()<std::string>();
    fn.template operator()<IntegerArgument>();
}

} // namespace

void registerCommands(std::shared_ptr<Application> const& app) {
    std::weak_ptr weak = app;

    auto& command = ll::command::CommandRegistrar::getInstance(false).getOrCreateCommand(
        "rbank",
        translate("cross-server bank", ll::i18n::getDefaultLocaleCode()),
        CommandPermissionLevel::Any
    );

    command.overload().execute([weak](CommandOrigin const& origin, CommandOutput& output) {
        auto user = player(origin);
        if (!user) {
            output.success("rbank list <xuid> | hist <xuid> | slot <xuid> | rate | rate set <server> <amount>");
            return;
        }

        if (auto app = weak.lock(); app && app->alive()) {
            showCards(app, *user);
        } else {
            output.error(translate("Bank is under maintenance", origin.getLocaleCode()));
        }
    });

    command.overload().text("create").execute([weak](CommandOrigin const& origin, CommandOutput& output) {
        auto user = player(origin);
        if (!user) {
            output.error(translate("Must be run by a player", origin.getLocaleCode()));
            return;
        }

        auto xuid = user->getXuid();
        dispatch(weak, origin, output, [xuid](Bank& bank) { return bank.create(xuid); });
    });

    for (auto action : {"list", "hist", "slot"}) {
        command.overload().text(action).execute([weak, action = std::string(action)](
                                                    CommandOrigin const& origin,
                                                    CommandOutput&       output
                                                ) {
            auto user = player(origin);
            if (!user) {
                output.error(translate("Must be run by a player; console must pass an XUID", origin.getLocaleCode()));
                return;
            }

            auto xuid = user->getXuid();
            dispatch(weak, origin, output, [xuid, action](Bank& bank) {
                return action == "list" ? bank.list(xuid) : action == "hist" ? bank.history(xuid) : bank.slots(xuid);
            });
        });
    }

    forBoth([&]<class T>() {
        for (auto action : {"info", "delete"}) {
            command.overload<CardParam<T>>().text(action).required("card").execute(
                [weak,
                 action =
                     std::string(action)](CommandOrigin const& origin, CommandOutput& output, CardParam<T> const& p) {
                    auto user = player(origin);
                    if (!user) {
                        output.error(translate("Must be run by a player", origin.getLocaleCode()));
                        return;
                    }

                    auto xuid = user->getXuid();
                    dispatch(weak, origin, output, [xuid, no = std::string(p.card), action](Bank& bank) {
                        return action == "info" ? bank.info(xuid, no) : bank.close(xuid, no);
                    });
                }
            );
        }

        for (auto action : {"deposit", "withdraw"}) {
            command.overload<AmountParam<T>>().text(action).required("card").required("amount").execute(
                [weak,
                 action =
                     std::string(action)](CommandOrigin const& origin, CommandOutput& output, AmountParam<T> const& p) {
                    auto user = player(origin);
                    if (!user) {
                        output.error(translate("Must be run by a player", origin.getLocaleCode()));
                        return;
                    }

                    try {
                        CashRequest request{user->getXuid(), p.card, amount(p.amount), {}};
                        dispatch(weak, origin, output, [request, action](Bank& bank) {
                            return action == "deposit" ? bank.deposit(request) : bank.withdraw(request);
                        });
                    } catch (Failure const& e) {
                        output.error(translate(e.detail, origin.getLocaleCode()));
                    }
                }
            );
        }

        for (auto action : {"freeze", "unfreeze"}) {
            command.overload<CardParam<T>>().text(action).required("card").execute(
                [weak,
                 frozen = std::string(action)
                       == "freeze"](CommandOrigin const& origin, CommandOutput& output, CardParam<T> const& p) {
                    if (!operator_(origin, output)) {
                        return;
                    }

                    dispatch(weak, origin, output, [no = std::string(p.card), frozen](Bank& bank) {
                        return bank.freeze(no, frozen);
                    });
                }
            );
        }

        forBoth([&]<class R>() {
            command.overload<PayParam<T, R>>()
                .text("pay")
                .required("card")
                .required("recipient")
                .required("amount")
                .execute([weak](CommandOrigin const& origin, CommandOutput& output, PayParam<T, R> const& p) {
                    auto user = player(origin);
                    if (!user) {
                        output.error(translate("Must be run by a player", origin.getLocaleCode()));
                        return;
                    }

                    try {
                        auto recipient = std::string(p.recipient);
                        if (auto info = ll::service::PlayerInfo::getInstance().fromName(recipient)) {
                            recipient = info->xuid;
                        }

                        PayRequest request{user->getXuid(), p.card, recipient, amount(p.amount), {}};
                        dispatch(weak, origin, output, [request](Bank& bank) { return bank.pay(request); });
                    } catch (Failure const& e) {
                        output.error(translate(e.detail, origin.getLocaleCode()));
                    }
                });
        });
    });

    command.overload().text("rate").execute([weak](CommandOrigin const& origin, CommandOutput& output) {
        dispatch(weak, origin, output, [](Bank& bank) { return bank.rates(); });
    });

    forBoth([&]<class T>() {
        command.overload<RateParam<T>>().text("rate").text("set").required("server").required("amount").execute(
            [weak](CommandOrigin const& origin, CommandOutput& output, RateParam<T> const& p) {
                if (!operator_(origin, output)) {
                    return;
                }

                try {
                    auto value = amount(p.amount);
                    auto by    = player(origin) ? player(origin)->getXuid() : std::string("console");
                    dispatch(weak, origin, output, [server = std::string(p.server), value, by](Bank& bank) {
                        return bank.setRate(server, value, by);
                    });
                } catch (Failure const& e) {
                    output.error(translate(e.detail, origin.getLocaleCode()));
                }
            }
        );
    });

    // 卡槽上限是热更配置，写入 config 表后下一次请求即生效。
    command.overload<SlotParam>().text("slot").text("set").required("count").execute(
        [weak](CommandOrigin const& origin, CommandOutput& output, SlotParam const& p) {
            if (!operator_(origin, output)) {
                return;
            }
            if (p.count < 1 || p.count > 100) {
                output.error(translate("Slot limit must be between 1 and 100", origin.getLocaleCode()));
                return;
            }

            auto by = player(origin) ? player(origin)->getXuid() : std::string("console");
            dispatch(weak, origin, output, [weak, count = p.count, by](Bank&) {
                auto app = weak.lock();
                require(app != nullptr, "UNAVAILABLE", "Bank is under maintenance");

                auto session = app->store().open();
                auto config  = session->config();
                config.slots = count;

                Transaction tx(*session);
                session->putConfig(config, by);
                tx.commit();

                return Result::success({
                    {"limit", count}
                });
            });
        }
    );

    for (auto action : {"list", "hist", "slot"}) {
        command.overload<TargetParam>().text(action).required("xuid").execute(
            [weak,
             action = std::string(action)](CommandOrigin const& origin, CommandOutput& output, TargetParam const& p) {
                if (!operator_(origin, output)) {
                    return;
                }

                dispatch(weak, origin, output, [xuid = std::string(p.xuid), action](Bank& bank) {
                    return action == "list" ? bank.list(xuid)
                         : action == "hist" ? bank.history(xuid)
                                            : bank.slots(xuid);
                });
            }
        );
    }
}

} // namespace rbank::adapter
