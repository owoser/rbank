#include "adapter/command/Commands.h"

#include "adapter/common/Presentation.h"
#include "bootstrap/di/Application.h"
#include "ll/api/form/CustomForm.h"
#include "ll/api/form/ModalForm.h"
#include "ll/api/form/SimpleForm.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/service/PlayerInfo.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

#include <algorithm>
#include <cstddef>
#include <ctime>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace rbank::adapter {

using namespace core;

namespace {

using Step  = std::function<void(Player&)>;
using Steps = std::vector<Step>;

std::string balanceLabel(Amount balance, std::string const& unit, std::string_view locale) {
    return translate(
        {
            "Balance {0} {1}",
            {std::to_string(balance), unit}
    },
        locale
    );
}

std::string title(std::string const& action, std::string_view locale) {
    return translate(action == "deposit" ? "Deposit" : action == "withdraw" ? "Withdraw" : "Transfer", locale);
}

/// 统一的行内格式：名称与值都由调用方给出，冒号交给译文决定。
std::string field(std::string_view name, std::string const& value, std::string_view locale) {
    return fill(translate("{0}: {1}", locale), {std::string{name}, value});
}

/// 余额与钱包始终成对出现在同一行，玩家一眼就能看出这次操作会动哪一边。
std::string funds(
    Amount             balance,
    Amount             wallet,
    std::string const& baseUnit,
    std::string const& currency,
    std::string_view   locale
) {
    return translate(
        {
            "Balance: {0}, Wallet: {1}",
            {std::to_string(balance) + " " + baseUnit, std::to_string(wallet) + " " + currency}
    },
        locale
    );
}

/// 开关回传的 bool 落到 variant 里是 uint64，取之前必须按实际存下的类型查。
bool flag(ll::form::CustomFormResult const& result, std::string const& name) {
    auto it = result->find(name);
    if (it == result->end()) {
        return false;
    }
    if (auto value = std::get_if<uint64>(&it->second)) {
        return *value != 0;
    }

    return false;
}

/// 一段信息写成一个多行文本，行距由原版 label 的行高给出，不再靠块间距拼凑。
std::string group(std::string_view locale, std::vector<std::pair<std::string, std::string>> const& items) {
    std::string text;
    for (auto const& [name, value] : items) {
        text += (text.empty() ? "" : "\n") + field(name, value, locale);
    }

    return text;
}

/// 汇率写成「1 基准单位 = 若干本服货币」，与玩家填金额的方向一致，举例时不必再换算。
std::string rateLine(Amount rate, std::string const& baseUnit, std::string const& currency, std::string_view locale) {
    return field(
        translate("Rate", locale),
        fill(translate("1 {0} = {1} {2}", locale), {baseUnit, std::to_string(rate), currency}),
        locale
    );
}

/// 只呈现玩家能核对的数字：单据 id 对玩家没有意义，原始键名和 JSON 也不该出现在界面上。
std::string receipt(Result const& result, std::string const& base, std::string const& local, std::string_view locale) {
    auto amountLine = [&](std::string_view name, std::string const& unit, char const* key) {
        if (!result.data.contains(key)) {
            return std::string{};
        }

        return field(translate(name, locale), std::to_string(result.data.at(key).get<Amount>()) + " " + unit, locale);
    };

    std::string text;
    for (auto const& line :
         {amountLine("Received", base, "credited_base"),
          amountLine("Amount", local, "local_amount"),
          amountLine("Amount", base, "base_amount"),
          amountLine("Fee", base, "fee_base")}) {
        if (line.empty()) {
            continue;
        }

        text += (text.empty() ? "" : "\n") + line;
    }

    return text;
}

/// 条目之间插原版分割线，上下留白由预设自身的 9px 面板给出。
void entries(ll::form::SimpleForm& form, std::vector<std::string> const& blocks) {
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        if (i > 0) {
            form.appendDivider();
        }
        form.appendLabel(blocks[i]);
    }
}

std::string stamp(Time time) {
    std::time_t value = static_cast<std::time_t>(time);
    std::tm     parts{};
    localtime_s(&parts, &value);

    char text[32] = {};
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &parts);

    return text;
}

std::string signedAmount(Amount value) { return value > 0 ? "+" + std::to_string(value) : std::to_string(value); }

std::string statusName(std::string const& status, std::string_view locale) {
    if (status == "FROZEN") {
        return translate("Frozen", locale);
    }
    if (status == "CLOSED") {
        return translate("Closed", locale);
    }

    return translate("Active", locale);
}

/// 注册了 sendTo 回调后，按钮自身的回调会被忽略，因此按钮与动作必须按下标同序登记。
void button(
    ll::form::SimpleForm&        form,
    Steps&                       steps,
    std::string const&           text,
    Step                         step,
    bootstrap::IconConfig const& icon = {}
) {
    if (icon.data.empty()) {
        form.appendButton(text);
    } else {
        form.appendButton(text, icon.data, icon.type);
    }
    steps.push_back(std::move(step));
}

/// 关闭手势与「返回」是同一件事：非根界面在玩家取消时重开上一层，不必退出后重新进菜单。
/// UserBusy 表示另一张界面正在顶替本界面，此时重建上一层会与新界面抢显示。
void send(Player& player, ll::form::SimpleForm& form, Steps steps, Step back = {}) {
    form.sendTo(
        player,
        [steps = std::move(steps), back = std::move(back)](Player& p, int selected, ll::form::FormCancelReason reason) {
            if (selected >= 0 && selected < static_cast<int>(steps.size())) {
                steps[selected](p);
                return;
            }
            if (back && reason == ModalFormCancelReason::UserClosed) {
                back(p);
            }
        }
    );
}

/// 转账目标的可选名单：在线优先，离线按名字补齐，够用即可，不对全量玩家开菜单。
/// 列表与输入框都只出现游戏内显示名，xuid 是内部标识，玩家既看不到也输不进来。
struct Choices {
    std::vector<std::string> options;
    std::vector<std::string> xuids;
};

Choices choices(std::string_view locale) {
    std::vector<std::string> online;
    if (auto level = ll::service::getLevel(); level) {
        level->forEachPlayer([&online](Player& p) {
            online.push_back(p.getXuid());

            return true;
        });
    }

    struct Row {
        std::string name, xuid;
        bool        online = false;
    };

    std::vector<Row> rows;
    for (auto const& entry : ll::service::PlayerInfo::getInstance().entries()) {
        auto at = std::find(online.begin(), online.end(), entry.xuid);
        rows.push_back({entry.name, entry.xuid, at != online.end()});
    }
    std::sort(rows.begin(), rows.end(), [](Row const& a, Row const& b) {
        return a.online != b.online ? a.online : a.name < b.name;
    });

    Choices result;
    result.options.push_back(translate("Unspecified", locale));
    result.xuids.emplace_back();
    for (std::size_t i = 0; i < rows.size() && result.options.size() < 20; ++i) {
        result.options.push_back(rows[i].name + " · " + translate(rows[i].online ? "Online" : "Offline", locale));
        result.xuids.push_back(rows[i].xuid);
    }

    return result;
}

void cardList(std::shared_ptr<Application> const&, Player&);
void cardInfo(std::shared_ptr<Application> const&, Player&, std::string);
void cashForm(std::shared_ptr<Application> const&, Player&, std::string, std::string);
void cashConfirm(
    std::shared_ptr<Application> const&,
    Player&,
    std::string,
    std::string,
    Amount,
    bool,
    Json const&,
    Result const&,
    std::string
);
void transferForm(std::shared_ptr<Application> const&, Player&, std::string);
void historyForm(std::shared_ptr<Application> const&, Player&, std::string);
void confirmClose(std::shared_ptr<Application> const&, Player&, std::string);

/// 业务查询统一走工作线程，结果回到主线程后再构建界面。
bool load(
    std::shared_ptr<Application> const&       app,
    Player&                                   player,
    std::function<Result(Bank&)>              task,
    std::function<void(Player&, Json const&)> done
) {
    auto xuid = player.getXuid();
    auto ok   = app->submit(std::move(task), [xuid, done = std::move(done)](Result result) {
        playerByXuid(xuid, [&](Player& p) {
            if (!result.ok()) {
                p.sendMessage(render(result, p.getLocaleCode()));
                return;
            }

            done(p, result.data);
        });
    });

    if (!ok) {
        player.sendMessage(translate("Bank is busy, try again later", player.getLocaleCode()));
    }

    return ok;
}

void openCard(std::shared_ptr<Application> const& app, Player& player) {
    auto xuid = player.getXuid();
    auto ok   = app->submit(
        [xuid](Bank& bank) { return bank.create(xuid); },
        [app, xuid](Result created) {
            playerByXuid(xuid, [&](Player& p) {
                if (!created.ok()) {
                    p.sendMessage(render(created, p.getLocaleCode()));
                }
                cardList(app, p);
            });
        }
    );

    if (!ok) {
        player.sendMessage(translate("Bank is busy, try again later", player.getLocaleCode()));
    }
}

void execute(
    std::shared_ptr<Application> const& app,
    Player&                             player,
    std::string                         no,
    std::string                         action,
    Amount                              value,
    std::string                         peer,
    std::string                         key
) {
    auto xuid = player.getXuid();
    auto ok   = app->submit(
        [xuid, no, action, value, key, peer](Bank& bank) {
            if (action == "deposit") {
                return bank.deposit({xuid, no, value, key});
            }
            if (action == "withdraw") {
                return bank.withdraw({xuid, no, value, key});
            }

            return bank.pay({xuid, no, peer, value, key});
        },
        [app, xuid](Result result) {
            playerByXuid(xuid, [&](Player& p) {
                auto locale = p.getLocaleCode();
                auto msg    = result.ok() ? receipt(result, app->baseUnit(), app->config().node.currency, locale)
                                            : translate(result.message, locale);

                // 回执逐行给出，聊天栏只显示一行，因此把换行压成空格。
                std::replace(msg.begin(), msg.end(), '\n', ' ');
                p.sendMessage(msg);
            });
        }
    );

    if (!ok) {
        player.sendMessage(translate("Bank is busy, try again later", player.getLocaleCode()));
    }
}

void cardList(std::shared_ptr<Application> const& app, Player& player) {
    auto xuid = player.getXuid();

    load(
        app,
        player,
        [xuid](Bank& bank) {
            auto cards = bank.list(xuid);
            if (!cards.ok() && cards.code != "EMPTY") {
                return cards;
            }

            auto slots = bank.slots(xuid);

            return Result::success({
                {"cards", cards.ok() ? std::move(cards.data) : Json::array()},
                {"used", slots.data.value("used", 0)},
                {"limit", slots.data.value("limit", 0)}
            });
        },
        [app](Player& p, Json const& data) {
            auto locale = p.getLocaleCode();
            auto icons  = app->config().icons;

            ll::form::SimpleForm form(translate("Cross-server bank", locale));
            Steps                steps;

            auto baseUnit = app->baseUnit();
            for (auto const& card : data.at("cards")) {
                auto no      = card.at("card_no").get<std::string>();
                auto balance = card.at("balance_base").get<Amount>();
                button(
                    form,
                    steps,
                    no + "\n" + balanceLabel(balance, baseUnit, locale),
                    [app, no](Player& player) { cardInfo(app, player, no); },
                    icons.cards
                );
            }

            if (data.value("used", 0) < data.value("limit", 0)) {
                button(
                    form,
                    steps,
                    translate("Open card", locale),
                    [app](Player& player) { openCard(app, player); },
                    icons.open
                );
            }

            send(p, form, std::move(steps));
        }
    );
}

void cardInfo(std::shared_ptr<Application> const& app, Player& player, std::string no) {
    auto xuid = player.getXuid();

    load(
        app,
        player,
        [xuid, no](Bank& bank) {
            auto info = bank.info(xuid, no);
            if (!info.ok()) {
                return info;
            }

            auto cash                 = bank.cash(xuid);
            info.data["wallet_local"] = cash.ok() ? cash.data["amount"] : Json{};

            return info;
        },
        [app, no](Player& p, Json const& data) {
            auto locale   = p.getLocaleCode();
            auto unit     = app->config().node.currency;
            auto baseUnit = app->baseUnit();
            auto wallet   = data.contains("wallet_local") && data.at("wallet_local").is_number()
                              ? std::to_string(data.at("wallet_local").get<Amount>()) + " " + unit
                              : translate("Unavailable", locale);
            auto balance  = std::to_string(data.value("balance_base", Amount{})) + " " + baseUnit;

            ll::form::SimpleForm form(translate(
                {
                    "Card {0} · {1}",
                    {std::to_string(data.value("slot", 0) + 1), data.value("card_no", no)}
            },
                locale
            ));
            form.setContent(field(translate("Status", locale), statusName(data.value("status", ""), locale), locale));

            auto const& icons = app->config().icons;

            Steps steps;
            button(
                form,
                steps,
                translate("Deposit", locale) + "\n" + field(translate("Wallet", locale), wallet, locale),
                [app, no](Player& player) { cashForm(app, player, no, "deposit"); },
                icons.deposit
            );
            button(
                form,
                steps,
                translate("Withdraw", locale) + "\n" + field(translate("Card balance", locale), balance, locale),
                [app, no](Player& player) { cashForm(app, player, no, "withdraw"); },
                icons.withdraw
            );
            button(
                form,
                steps,
                translate("Transfer", locale),
                [app, no](Player& player) { transferForm(app, player, no); },
                icons.transfer
            );
            button(
                form,
                steps,
                translate("History", locale),
                [app, no](Player& player) { historyForm(app, player, no); },
                icons.history
            );
            button(
                form,
                steps,
                translate("Close bank card", locale),
                [app, no](Player& player) { confirmClose(app, player, no); },
                icons.closeCard
            );
            button(
                form,
                steps,
                translate("Back", locale),
                [app](Player& player) { cardList(app, player); },
                icons.back
            );
            button(form, steps, translate("Close", locale), [](Player&) {}, icons.close);

            send(p, form, std::move(steps), [app](Player& q) { cardList(app, q); });
        }
    );
}

/// 第一步：先给出当前余额与钱包，再让玩家填金额；金额校验通过后进入确认预览。
void cashForm(std::shared_ptr<Application> const& app, Player& player, std::string no, std::string action) {
    auto xuid = player.getXuid();

    load(
        app,
        player,
        [xuid, no](Bank& bank) {
            auto info = bank.info(xuid, no);
            if (!info.ok()) {
                return info;
            }

            auto cash                 = bank.cash(xuid);
            info.data["wallet_local"] = cash.ok() ? cash.data["amount"] : Json{};

            auto rate         = bank.localRate();
            info.data["rate"] = rate.ok() ? rate.data["rate"] : Json{};

            return info;
        },
        [app, no, action](Player& p, Json const& data) {
            auto locale   = p.getLocaleCode();
            auto unit     = app->config().node.currency;
            auto baseUnit = app->baseUnit();
            auto rate     = data.value("rate", Amount{});
            auto key      = app->requestKey();

            ll::form::CustomForm form(title(action, locale));
            form.appendLabel(funds(
                data.value("balance_base", Amount{}),
                data.value("wallet_local", Amount{}),
                baseUnit,
                unit,
                locale
            ));
            if (rate > 0) {
                form.appendLabel(rateLine(rate, baseUnit, unit, locale));
            }

            form.appendDivider();
            form.appendInput("amount", translate("Amount", locale));
            form.appendToggle("base", translate("Automatic conversion", locale));
            form.setSubmitButton(translate("Submit", locale));

            form.sendTo(
                p,
                [app, no, action, locale, rate, key, data](
                    Player&                           q,
                    ll::form::CustomFormResult const& result,
                    ll::form::FormCancelReason        reason
                ) {
                    if (!result) {
                        if (reason == ModalFormCancelReason::UserClosed) {
                            cardInfo(app, q, no);
                        }
                        return;
                    }

                    Amount value{};
                    try {
                        value = amount(std::get<std::string>(result->at("amount")));
                    } catch (Failure const& e) {
                        q.sendMessage(translate(e.detail, locale));
                        cashForm(app, q, no, action);

                        return;
                    } catch (...) {
                        q.sendMessage(translate("Amount must be a positive integer", locale));
                        cashForm(app, q, no, action);

                        return;
                    }

                    // 按基准单位输入时要折回本服货币再下单：存款与取款都以本服货币计价。
                    auto baseInput = flag(result, "base");
                    auto local     = value;
                    if (baseInput) {
                        if (rate <= 0) {
                            q.sendMessage(translate("Local rate is not configured", locale));
                            cashForm(app, q, no, action);

                            return;
                        }
                        local = times(value, rate);
                    }

                    auto xuid = q.getXuid();
                    auto dir  = action == "deposit" ? Direction::Deposit : Direction::Withdraw;
                    auto ok   = app->submit(
                        [xuid, no, local, dir](Bank& bank) { return bank.quote(xuid, no, local, dir); },
                        [app, xuid, no, action, local, baseInput, data, key](Result quote) {
                            playerByXuid(xuid, [&](Player& r) {
                                if (!quote.ok()) {
                                    r.sendMessage(render(quote, r.getLocaleCode()));
                                    cashForm(app, r, no, action);

                                    return;
                                }

                                cashConfirm(app, r, no, action, local, baseInput, data, quote, key);
                            });
                        }
                    );

                    if (!ok) {
                        q.sendMessage(translate("Bank is busy, try again later", locale));
                    }
                }
            );
        }
    );
}

/// 第二步：模态框给出确认后的余额与钱包，只有确认才真正下单。
void cashConfirm(
    std::shared_ptr<Application> const& app,
    Player&                             player,
    std::string                         no,
    std::string                         action,
    Amount                              local,
    bool                                baseInput,
    Json const&                         before,
    Result const&                       quote,
    std::string                         key
) {
    auto locale = player.getLocaleCode();
    auto unit   = app->config().node.currency;
    auto base   = app->baseUnit();

    auto show = [&](Json const& data, char const* key) {
        return std::to_string(data.value(key, Amount{})) + " " + base;
    };
    auto wallet = [&](Json const& data, char const* key) {
        return std::to_string(data.value(key, Amount{})) + " " + unit;
    };

    // 玩家可能按基准单位输入，确认框仍按他选择的单位复述，避免换算后对不上号。
    auto amount = baseInput ? std::to_string(local / quote.data.value("rate", Amount{1})) + " " + base
                            : std::to_string(local) + " " + unit;

    ll::form::ModalForm form(
        title(action, locale),
        group(
            locale,
            {
                {translate("Amount", locale), amount},
                {translate("Rate", locale),
                 fill(
                     translate("1 {0} = {1} {2}", locale),
                 {base, std::to_string(quote.data.value("rate", Amount{})), unit}
                 )},
                {translate("Card balance", locale),
                 show(before, "balance_base") + " > " + show(quote.data, "balance_base")},
                {translate("Wallet", locale),
                 wallet(before, "wallet_local") + " > " + wallet(quote.data, "wallet_local")},
    }
        ),
        translate("Confirm", locale),
        translate("Cancel", locale)
    );
    form.sendTo(
        player,
        [app, no, action, local, key](Player& p, ll::form::ModalFormResult choice, ll::form::FormCancelReason reason) {
            if (!choice) {
                if (reason == ModalFormCancelReason::UserClosed) {
                    cashForm(app, p, no, action);
                }
                return;
            }
            if (*choice != ll::form::ModalFormSelectedButton::Upper) {
                cashForm(app, p, no, action);

                return;
            }

            execute(app, p, no, action, local, {}, key);
        }
    );
}

void transferForm(std::shared_ptr<Application> const& app, Player& player, std::string no) {
    auto locale = player.getLocaleCode();
    auto key    = app->requestKey();
    auto people = choices(locale);
    auto unit   = app->baseUnit();

    ll::form::CustomForm form(translate("Transfer", locale));
    form.appendInput("card", translate("Recipient card number", locale));
    form.appendInput("player", translate("Recipient player name", locale));
    form.appendDropdown("pick", translate("Select a player", locale), people.options);
    form.appendLabel(translate("Transfer tips", locale));
    form.appendInput("amount", fill(translate("Amount ({0})", locale), {unit}));
    form.setSubmitButton(translate("Submit", locale));

    form.sendTo(
        player,
        [app,
         no,
         locale,
         key,
         people](Player& p, ll::form::CustomFormResult const& result, ll::form::FormCancelReason reason) {
            if (!result) {
                if (reason == ModalFormCancelReason::UserClosed) {
                    cardInfo(app, p, no);
                }
                return;
            }

            auto retry = [app, no, locale](Player& q, Message const& message) {
                q.sendMessage(translate(message, locale));
                transferForm(app, q, no);
            };

            Amount value{};
            try {
                value = amount(std::get<std::string>(result->at("amount")));
            } catch (Failure const& e) {
                retry(p, e.detail);

                return;
            } catch (...) {
                retry(p, "Amount must be a positive integer");

                return;
            }

            // 三种指定方式按「卡号 → 玩家名 → 名单」取第一个非空值，空则要求重填。
            auto peer = std::get<std::string>(result->at("card"));
            if (peer.empty()) {
                peer = std::get<std::string>(result->at("player"));
                if (!peer.empty()) {
                    // 界面收的是游戏显示名，内部才用 xuid；名字查不到说明玩家从没进过服。
                    auto info = ll::service::PlayerInfo::getInstance().fromName(peer);
                    if (!info) {
                        retry(p, "Recipient not found");

                        return;
                    }
                    peer = info->xuid;
                }
            }
            if (peer.empty()) {
                auto picked = std::get<std::string>(result->at("pick"));
                auto at     = std::find(people.options.begin(), people.options.end(), picked);
                if (at != people.options.end()) {
                    peer = people.xuids[static_cast<std::size_t>(std::distance(people.options.begin(), at))];
                }
            }
            if (peer.empty()) {
                retry(p, "Enter a recipient");

                return;
            }

            execute(app, p, no, "pay", value, peer, key);
        }
    );
}

void historyForm(std::shared_ptr<Application> const& app, Player& player, std::string no) {
    auto xuid = player.getXuid();

    load(
        app,
        player,
        [xuid](Bank& bank) { return bank.history(xuid, 5); },
        [app, no](Player& p, Json const& data) {
            auto locale = p.getLocaleCode();
            auto unit   = app->baseUnit();

            ll::form::SimpleForm     form(translate("History", locale));
            std::vector<std::string> blocks;
            for (auto const& entry : data) {
                blocks.push_back(group(
                    locale,
                    {
                        {translate("Kind",          locale), entry.value("kind",                    std::string{})         },
                        {translate("Change",        locale), signedAmount(entry.value("delta_base", Amount{})) + " " + unit},
                        {translate("Fee",           locale), std::to_string(entry.value("fee_base", Amount{})) + " " + unit},
                        {translate("Balance after", locale),
                         std::to_string(entry.value("balance_after",                                Amount{})) + " " + unit},
                        {translate("Rate",          locale), std::to_string(entry.value("rate",     Amount{}))             },
                        {translate("Server",        locale), entry.value("server_id",               std::string{})         },
                        {translate("Time",          locale), stamp(entry.value("created_at",        Time{}))               }
                }
                ));
            }

            if (blocks.empty()) {
                form.setContent(translate("No history", locale));
            } else {
                entries(form, blocks);
            }

            auto const& icons = app->config().icons;

            Steps steps;
            button(
                form,
                steps,
                translate("Back", locale),
                [app, no](Player& player) { cardInfo(app, player, no); },
                icons.back
            );
            button(form, steps, translate("Close", locale), [](Player&) {}, icons.close);
            send(p, form, std::move(steps), [app, no](Player& q) { cardInfo(app, q, no); });
        }
    );
}

void confirmClose(std::shared_ptr<Application> const& app, Player& player, std::string no) {
    auto locale = player.getLocaleCode();

    ll::form::ModalForm form(
        translate("Close bank card", locale),
        no + "\n" + translate("The card can no longer be used after closing", locale),
        translate("Cancel a card", locale),
        translate("Cancel", locale)
    );
    form.sendTo(player, [app, no](Player& p, ll::form::ModalFormResult choice, ll::form::FormCancelReason reason) {
        if (!choice) {
            if (reason == ModalFormCancelReason::UserClosed) {
                cardInfo(app, p, no);
            }
            return;
        }
        if (*choice != ll::form::ModalFormSelectedButton::Upper) {
            cardInfo(app, p, no);

            return;
        }

        auto xuid = p.getXuid();
        auto ok   = app->submit(
            [xuid, no](Bank& bank) { return bank.close(xuid, no); },
            [app, xuid, no](Result result) {
                playerByXuid(xuid, [&](Player& q) {
                    if (!result.ok()) {
                        q.sendMessage(render(result, q.getLocaleCode()));

                        return;
                    }

                    q.sendMessage(field(translate("Close bank card", q.getLocaleCode()), no, q.getLocaleCode()));
                });
            }
        );

        if (!ok) {
            p.sendMessage(translate("Bank is busy, try again later", p.getLocaleCode()));
        }
    });
}

} // namespace

void showCards(std::shared_ptr<Application> const& app, Player& p) { cardList(app, p); }

void onJoin(std::shared_ptr<Application> const& app, Player& p) {
    auto xuid = p.getXuid();

    app->join(xuid, [app, xuid](Result r) {
        if (!r.ok()) {
            return;
        }

        playerByXuid(xuid, [&](Player& player) {
            auto locale = player.getLocaleCode();

            ll::form::SimpleForm form(translate("Cross-server bank", locale));

            auto unit  = app->baseUnit();
            auto cards = r.data.value("cards", Json::array());
            if (cards.empty()) {
                form.setContent(translate("No bank card opened yet", locale));
            } else {
                std::vector<std::string> blocks;
                for (auto const& card : cards) {
                    blocks.push_back(group(
                        locale,
                        {
                            {translate("Card number",  locale), card.value("card_no", std::string{})         },
                            {translate("Card balance", locale),
                             std::to_string(card.value("balance_base",                Amount{})) + " " + unit}
                    }
                    ));
                }
                entries(form, blocks);
            }

            auto history = r.data.value("history", Json::array());
            if (!history.empty()) {
                std::vector<std::string> blocks;
                for (auto const& entry : history) {
                    blocks.push_back(group(
                        locale,
                        {
                            {translate("Kind",   locale), entry.value("kind",             std::string{})         },
                            {translate("Change", locale),
                             signedAmount(entry.value("delta_base",                       Amount{})) + " " + unit},
                            {translate("Time",   locale), stamp(entry.value("created_at", Time{}))               }
                    }
                    ));
                }

                form.appendHeader(translate("History", locale));
                entries(form, blocks);
            }

            Steps steps;
            button(
                form,
                steps,
                translate("Bank cards", locale),
                [app](Player& player) { cardList(app, player); },
                app->config().icons.cards
            );
            send(player, form, std::move(steps));
        });
    });
}

} // namespace rbank::adapter
