#include "adapter/command/Commands.h"

#include "adapter/common/Presentation.h"
#include "bootstrap/di/Application.h"
#include "ll/api/form/CustomForm.h"
#include "ll/api/form/ModalForm.h"
#include "ll/api/form/SimpleForm.h"

#include <variant>

namespace rbank::adapter {

using namespace core;

namespace {

/// 一次打开表单生成一个请求键，其所有回调复用；指令每次敲击都是新请求，不参与去重。
void amountForm(std::shared_ptr<Application> const& app, Player& p, std::string card, std::string action) {
    auto          key  = app->requestKey();
    std::weak_ptr weak = app;

    ll::form::CustomForm form(action == "deposit" ? "存款" : action == "withdraw" ? "取款" : "转账");
    if (action == "pay") {
        form.appendInput("recipient", "收款卡号或玩家 ID");
    }
    form.appendInput("amount", action == "pay" ? "金额（base）" : "金额（本服货币）");

    form.sendTo(
        p,
        [weak,
         key,
         card = std::move(card),
         action](Player& player, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
            if (!result) {
                return;
            }

            auto app = weak.lock();
            if (!app || !app->alive()) {
                return;
            }

            try {
                auto value = std::get<std::string>(result->at("amount"));
                auto n     = amount(value);
                auto xuid  = player.getXuid();
                auto peer  = action == "pay" ? std::get<std::string>(result->at("recipient")) : std::string{};

                auto queued = app->submit(
                    [xuid, card, action, n, key, peer](Bank& bank) {
                        if (action == "deposit") {
                            return bank.deposit({xuid, card, n, key});
                        }
                        if (action == "withdraw") {
                            return bank.withdraw({xuid, card, n, key});
                        }

                        return bank.pay({xuid, card, peer, n, key});
                    },
                    [xuid](Result r) { playerByXuid(xuid, [&](Player& p) { p.sendMessage(render(r)); }); }
                );
                if (!queued) {
                    player.sendMessage("银行繁忙，请稍后重试");
                }
            } catch (Failure const& e) {
                player.sendMessage(e.what());
            } catch (...) {
                player.sendMessage("请输入有效金额");
            }
        }
    );
}

void cardMenu(std::shared_ptr<Application> const& app, Player& p, std::string no) {
    std::weak_ptr weak = app;
    auto          xuid = p.getXuid();

    app->submit(
        [xuid, no](Bank& bank) { return bank.info(xuid, no); },
        [weak, xuid, no](Result info) {
            auto app = weak.lock();
            if (!app) {
                return;
            }

            playerByXuid(xuid, [&](Player& player) {
                if (!info.ok()) {
                    player.sendMessage(render(info));
                    return;
                }

                ll::form::SimpleForm form(no, render(info));
                for (auto const& [label, action] : std::vector<std::pair<std::string, std::string>>{
                         {"存款", "deposit" },
                         {"取款", "withdraw"},
                         {"转账", "pay"     }
                }) {
                    form.appendButton(label, [weak, no, action](Player& p) {
                        if (auto app = weak.lock(); app && app->alive()) {
                            amountForm(app, p, no, action);
                        }
                    });
                }

                form.appendButton("流水", [weak, xuid](Player& p) {
                    auto app = weak.lock();
                    if (!app || !app->alive()) {
                        return;
                    }

                    if (!app->submit(
                            [xuid](Bank& bank) { return bank.history(xuid); },
                            [xuid](Result r) {
                                playerByXuid(xuid, [&](Player& p) {
                                    ll::form::SimpleForm("流水", render(r)).sendTo(p);
                                });
                            }
                        )) {
                        p.sendMessage("银行繁忙，请稍后重试");
                    }
                });

                form.appendButton("注销", [weak, xuid, no](Player& p) {
                    ll::form::ModalForm("注销银行卡", no, "注销", "取消")
                        .sendTo(
                            p,
                            [weak, xuid, no](Player& p, ll::form::ModalFormResult choice, ll::form::FormCancelReason) {
                                if (!choice || *choice != ll::form::ModalFormSelectedButton::Upper) {
                                    return;
                                }

                                auto app = weak.lock();
                                if (!app || !app->alive()) {
                                    return;
                                }

                                if (!app->submit(
                                        [xuid, no](Bank& bank) { return bank.close(xuid, no); },
                                        [xuid](Result r) {
                                            playerByXuid(xuid, [&](Player& p) { p.sendMessage(render(r)); });
                                        }
                                    )) {
                                    p.sendMessage("银行繁忙，请稍后重试");
                                }
                            }
                        );
                });

                form.sendTo(player);
            });
        }
    );
}

} // namespace

void showCards(std::shared_ptr<Application> const& app, Player& p) {
    auto          xuid = p.getXuid();
    std::weak_ptr weak = app;

    if (!app->submit(
            [xuid](Bank& bank) { return bank.list(xuid); },
            [weak, xuid](Result r) {
                auto app = weak.lock();
                if (!app) {
                    return;
                }

                playerByXuid(xuid, [&](Player& player) {
                    if (!r.ok()) {
                        player.sendMessage(render(r));
                        return;
                    }

                    ll::form::SimpleForm form("跨服银行");
                    for (auto const& card : r.data) {
                        auto no = card.at("card_no").get<std::string>();
                        form.appendButton(
                            no + "\n余额 " + std::to_string(card.at("balance_base").get<Amount>()) + " base",
                            [weak, no](Player& p) {
                                if (auto app = weak.lock(); app && app->alive()) {
                                    cardMenu(app, p, no);
                                }
                            }
                        );
                    }

                    form.appendButton("开户", [weak, xuid](Player& p) {
                        auto app = weak.lock();
                        if (!app || !app->alive()) {
                            return;
                        }

                        if (!app->submit(
                                [xuid](Bank& bank) { return bank.create(xuid); },
                                [weak, xuid](Result created) {
                                    playerByXuid(xuid, [&](Player& p) {
                                        p.sendMessage(render(created));
                                        if (auto app = weak.lock(); app && created.ok()) {
                                            showCards(app, p);
                                        }
                                    });
                                }
                            )) {
                            p.sendMessage("银行繁忙，请稍后重试");
                        }
                    });

                    form.sendTo(player);
                });
            }
        )) {
        p.sendMessage("银行繁忙，请稍后重试");
    }
}

void onJoin(std::shared_ptr<Application> const& app, Player& p) {
    auto          xuid = p.getXuid();
    std::weak_ptr weak = app;

    app->join(xuid, [weak, xuid](Result r) {
        if (!r.ok()) {
            return;
        }

        playerByXuid(xuid, [&](Player& player) {
            ll::form::SimpleForm form("跨服银行", render(r));
            form.appendButton("银行卡", [weak](Player& p) {
                if (auto app = weak.lock(); app && app->alive()) {
                    showCards(app, p);
                }
            });

            form.sendTo(player);
        });
    });
}

} // namespace rbank::adapter
