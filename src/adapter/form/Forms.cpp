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

std::string balanceLabel(Amount balance, std::string_view locale) {
    return translate({"Balance {0} base", {std::to_string(balance)}}, locale);
}

std::string title(std::string const& action, std::string_view locale) {
    return translate(action == "deposit" ? "Deposit" : action == "withdraw" ? "Withdraw" : "Transfer", locale);
}

/// 一次打开表单生成一个请求键，其所有回调复用；指令每次敲击都是新请求，不参与去重。
void amountForm(std::shared_ptr<Application> const& app, Player& p, std::string card, std::string action) {
    auto          key  = app->requestKey();
    std::weak_ptr weak = app;

    auto                 locale = p.getLocaleCode();
    ll::form::CustomForm form(title(action, locale));
    if (action == "pay") {
        form.appendInput("recipient", translate("Recipient card number or player ID", locale));
    }
    form.appendInput("amount", translate(action == "pay" ? "Amount (base)" : "Amount (local currency)", locale));

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
                    [xuid](Result r) {
                        playerByXuid(xuid, [&](Player& p) { p.sendMessage(render(r, p.getLocaleCode())); });
                    }
                );
                if (!queued) {
                    player.sendMessage(translate("Bank is busy, try again later", player.getLocaleCode()));
                }
            } catch (Failure const& e) {
                player.sendMessage(translate(e.detail, player.getLocaleCode()));
            } catch (...) {
                player.sendMessage(translate("Enter a valid amount", player.getLocaleCode()));
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
                auto locale = player.getLocaleCode();
                if (!info.ok()) {
                    player.sendMessage(render(info, locale));
                    return;
                }

                ll::form::SimpleForm form(no, render(info, locale));
                for (auto const& [label, action] : std::vector<std::pair<std::string, std::string>>{
                         {"Deposit",  "deposit" },
                         {"Withdraw", "withdraw"},
                         {"Transfer", "pay"     }
                }) {
                    form.appendButton(translate(label, locale), [weak, no, action](Player& p) {
                        if (auto app = weak.lock(); app && app->alive()) {
                            amountForm(app, p, no, action);
                        }
                    });
                }

                form.appendButton(translate("History", locale), [weak, xuid](Player& p) {
                    auto app = weak.lock();
                    if (!app || !app->alive()) {
                        return;
                    }

                    if (!app->submit(
                            [xuid](Bank& bank) { return bank.history(xuid); },
                            [xuid](Result r) {
                                playerByXuid(xuid, [&](Player& p) {
                                    ll::form::SimpleForm(
                                        translate("History", p.getLocaleCode()),
                                        render(r, p.getLocaleCode())
                                    )
                                        .sendTo(p);
                                });
                            }
                        )) {
                        p.sendMessage(translate("Bank is busy, try again later", p.getLocaleCode()));
                    }
                });

                form.appendButton(translate("Close", locale), [weak, xuid, no](Player& p) {
                    ll::form::ModalForm(
                        translate("Close bank card", p.getLocaleCode()),
                        no,
                        translate("Close", p.getLocaleCode()),
                        translate("Cancel", p.getLocaleCode())
                    )
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
                                            playerByXuid(xuid, [&](Player& p) {
                                                p.sendMessage(render(r, p.getLocaleCode()));
                                            });
                                        }
                                    )) {
                                    p.sendMessage(translate("Bank is busy, try again later", p.getLocaleCode()));
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
                    auto locale = player.getLocaleCode();
                    if (!r.ok()) {
                        player.sendMessage(render(r, locale));
                        return;
                    }

                    ll::form::SimpleForm form(translate("Cross-server bank", locale));
                    for (auto const& card : r.data) {
                        auto no = card.at("card_no").get<std::string>();
                        form.appendButton(
                            no + "\n" + balanceLabel(card.at("balance_base").get<Amount>(), locale),
                            [weak, no](Player& p) {
                                if (auto app = weak.lock(); app && app->alive()) {
                                    cardMenu(app, p, no);
                                }
                            }
                        );
                    }

                    form.appendButton(translate("Open card", locale), [weak, xuid](Player& p) {
                        auto app = weak.lock();
                        if (!app || !app->alive()) {
                            return;
                        }

                        if (!app->submit(
                                [xuid](Bank& bank) { return bank.create(xuid); },
                                [weak, xuid](Result created) {
                                    playerByXuid(xuid, [&](Player& p) {
                                        p.sendMessage(render(created, p.getLocaleCode()));
                                        if (auto app = weak.lock(); app && created.ok()) {
                                            showCards(app, p);
                                        }
                                    });
                                }
                            )) {
                            p.sendMessage(translate("Bank is busy, try again later", p.getLocaleCode()));
                        }
                    });

                    form.sendTo(player);
                });
            }
        )) {
        p.sendMessage(translate("Bank is busy, try again later", p.getLocaleCode()));
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
            auto                 locale = player.getLocaleCode();
            ll::form::SimpleForm form(translate("Cross-server bank", locale), render(r, locale));
            form.appendButton(translate("Bank cards", locale), [weak](Player& p) {
                if (auto app = weak.lock(); app && app->alive()) {
                    showCards(app, p);
                }
            });

            form.sendTo(player);
        });
    });
}

} // namespace rbank::adapter
