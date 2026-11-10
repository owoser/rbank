#include "core/service/Bank.h"

namespace rbank::core {

namespace {

/// card.balance_base 是 lot 的冗余汇总，两者不一致说明资金账已损坏，必须停手。
void verify(Card const& c, std::vector<Lot> const& lots) {
    Amount sum = 0;
    for (auto const& lot : lots) {
        sum = plus(sum, lot.remain);
    }

    require(sum == c.balance, "INVARIANT", "银行卡批次与余额不一致");
}

} // namespace

Card Bank::owned(Session& s, std::string const& xuid, std::string const& no, bool active) {
    auto c = s.card(no);
    require(c && c->xuid == xuid && c->status != "CLOSED", "CARD", "未找到你的银行卡");
    require(!active || c->status == "ACTIVE", "FROZEN", "银行卡已冻结");

    return *c;
}

void Bank::ready(Session& s, std::string const& xuid) {
    identity(xuid);

    auto cfg = s.config();
    require(cfg.enabled, "MAINTENANCE", "银行维护中");

    auto nodes = s.nodes();
    auto self  = std::find_if(nodes.begin(), nodes.end(), [this](auto const& n) { return n.id == node.id; });
    require(
        self != nodes.end() && self->name == node.name && self->currency == node.currency && self->status == "ONLINE",
        "NODE",
        "银行节点身份异常，当前只读"
    );
    require(!s.unsettled(xuid), "PENDING", "有待确认的资金操作，请稍后重试");
}

Amount Bank::rate(Session& s) {
    for (auto const& r : s.rates(clock.now())) {
        if (r.server == node.id && r.value > 0) {
            return r.value;
        }
    }

    throw Failure("RATE", "本服汇率未配置");
}

void Bank::limits(Session& s, HotConfig const& cfg, std::string const& xuid, Direction dir, Amount base) {
    require(base > 0 && base <= cfg.maxSingle, "LIMIT", "金额超过单笔限额");

    auto today = clock.now() / 86400 * 86400;
    require(plus(s.daily(xuid, dir, today), base) <= cfg.maxDaily, "LIMIT", "金额超过日限额");
}

void Bank::ledger(
    Session&           s,
    Intent const&      i,
    Card const&        c,
    Amount             delta,
    std::string const& kind,
    std::string const& peer
) {
    LedgerEntry entry;
    entry.id      = ids.next();
    entry.xuid    = c.xuid;
    entry.card    = c.no;
    entry.kind    = kind;
    entry.delta   = delta;
    entry.fee     = i.fee;
    entry.balance = c.balance;
    entry.rate    = i.rate;
    entry.local   = i.local;
    entry.server  = i.server;
    entry.peer    = peer;
    entry.intent  = i.id;
    entry.created = clock.now();
    s.appendLedger(entry);
}

Result Bank::create(std::string const& xuid) {
    identity(xuid);

    auto        s = db.open({xuid});
    Transaction tx(*s);
    ready(*s, xuid);

    auto slot = SlotService::available(s->cards(xuid), s->config().slots);
    for (int retry = 0; retry < 5; ++retry) {
        Card c{cardgen.generate(xuid, *s), xuid};
        c.slot = slot;
        if (s->insertCard(c)) {
            tx.commit();

            return Result::success({
                {"card_no", c.no  },
                {"slot",    c.slot}
            });
        }
    }

    throw Failure("CARD_GENERATION", "卡号生成失败，请联系管理员调整规则");
}

Result Bank::close(std::string const& xuid, std::string const& no) {
    auto        s = db.open({xuid});
    Transaction tx(*s);
    ready(*s, xuid);

    auto c = owned(*s, xuid, no);
    require(c.balance == 0, "BALANCE", "余额必须为 0 才能删卡");
    verify(c, s->lots(c.no));

    c.status = "CLOSED";
    ++c.version;
    s->saveCard(c);
    tx.commit();

    return Result::success({
        {"card_no", c.no}
    });
}

Result Bank::list(std::string const& xuid) {
    identity(xuid);

    auto s     = db.open();
    auto cards = s->cards(xuid);
    Json data  = Json::array();
    for (auto const& c : cards) {
        data.push_back({
            {"card_no",      c.no     },
            {"slot",         c.slot   },
            {"balance_base", c.balance},
            {"status",       c.status }
        });
    }

    if (cards.empty()) {
        return Result::fail("EMPTY", "尚未开通银行卡");
    }

    return Result::success(std::move(data));
}

Result Bank::info(std::string const& xuid, std::string const& no) {
    auto s = db.open();
    auto c = owned(*s, xuid, no, false);

    return Result::success({
        {"card_no",      c.no     },
        {"slot",         c.slot   },
        {"balance_base", c.balance},
        {"status",       c.status }
    });
}

Result Bank::history(std::string const& xuid, int count) {
    identity(xuid);

    auto s    = db.open();
    Json data = Json::array();
    for (auto const& l : s->history(xuid, std::clamp(count, 1, 100))) {
        data.push_back({
            {"kind",          l.kind   },
            {"delta_base",    l.delta  },
            {"fee_base",      l.fee    },
            {"balance_after", l.balance},
            {"rate",          l.rate   },
            {"server_id",     l.server },
            {"created_at",    l.created}
        });
    }

    return Result::success(std::move(data));
}

void Bank::settleDeposit(Session& s, Intent const& i, State from, std::string const& note) {
    Transaction tx(s);
    require(s.cas(i.id, from, State::Settled, clock.now(), note), "TAKEN", "该操作已处理");

    auto c      = owned(s, i.xuid, i.card, false);
    auto amount = i.base - i.fee;
    require(amount > 0, "INVARIANT", "存款入账金额异常");
    verify(c, s.lots(c.no));

    c.balance = plus(c.balance, amount);
    ++c.version;
    Lot lot;
    lot.id       = ids.next();
    lot.card     = c.no;
    lot.origin   = i.server;
    lot.original = amount;
    lot.remain   = amount;
    lot.created  = clock.now();
    s.insertLot(lot);
    s.saveCard(c);
    ledger(s, i, c, amount, "DEPOSIT");
    tx.commit();
}

Result Bank::deposit(CashRequest const& q) {
    auto s   = db.open({q.xuid});
    auto cfg = s->config();
    ready(*s, q.xuid);
    require(cfg.deposits, "DISABLED", "存款暂未开放");

    auto c    = owned(*s, q.xuid, q.card);
    auto r    = rate(*s);
    auto base = FeePolicy::convert(q.local, r);
    limits(*s, cfg, q.xuid, Direction::Deposit, base);

    auto fee = FeePolicy::calculate(base, cfg.deposit);
    require(fee < base, "FEE", "手续费超过本金");
    verify(c, s->lots(c.no));
    fits(c.balance, base - fee);

    auto balance = money.get(q.xuid);
    require(balance && *balance >= q.local, "LEGACY_BALANCE", "本服余额不足或原版经济不可用");

    Intent i{};
    i.id         = ids.next();
    i.xuid       = q.xuid;
    i.card       = q.card;
    i.server     = node.id;
    i.base       = base;
    i.fee        = fee;
    i.local      = q.local;
    i.rate       = r;
    i.requestKey = q.key;
    i.created    = clock.now();
    i.updated    = i.created;
    {
        Transaction tx(*s);
        s->insertIntent(i);
        tx.commit();
    }

    // 先扣来源：原版扣款失败即终止，无任何副作用。
    auto outcome = money.reduce(q.xuid, q.local);
    if (outcome == MoneyOutcome::Failed) {
        Transaction tx(*s);
        s->cas(i.id, State::PendingLegacy, State::RolledBack, clock.now(), "LegacyMoney Reduce rejected");
        tx.commit();

        throw Failure("LEGACY", "原版扣款失败，存款未执行");
    }
    if (outcome == MoneyOutcome::Unknown) {
        throw Failure("PENDING", "扣款结果待确认，单号 " + i.id);
    }

    {
        Transaction tx(*s);
        require(s->cas(i.id, State::PendingLegacy, State::PendingDb, clock.now()), "TAKEN", "该操作已处理");
        tx.commit();
    }

    settleDeposit(*s, i, State::PendingDb);

    return Result::success({
        {"intent_id",     i.id      },
        {"credited_base", base - fee},
        {"fee_base",      fee       }
    });
}

void Bank::undoWithdrawal(Session& s, Intent const& i, State from, std::string const& note) {
    Transaction tx(s);
    require(s.cas(i.id, from, State::RolledBack, clock.now(), note), "TAKEN", "该操作已处理");

    auto c = owned(s, i.xuid, i.card, false);
    verify(c, s.lots(c.no));

    Amount restored = 0;
    for (auto const& slice : i.slices) {
        restored = plus(restored, slice.amount);
        Lot lot;
        lot.id       = ids.next();
        lot.card     = c.no;
        lot.origin   = slice.origin;
        lot.original = slice.amount;
        lot.remain   = slice.amount;
        lot.created  = slice.created;
        s.insertLot(lot);
    }
    require(restored == plus(i.base, i.fee), "INVARIANT", "补偿批次不完整");

    c.balance = plus(c.balance, restored);
    ++c.version;
    s.saveCard(c);
    ledger(s, i, c, restored, "WITHDRAW_ROLLBACK");
    tx.commit();
}

Result Bank::withdraw(CashRequest const& q) {
    auto s   = db.open({q.xuid});
    auto cfg = s->config();
    ready(*s, q.xuid);
    require(cfg.withdrawals, "DISABLED", "取款暂未开放");

    auto c    = owned(*s, q.xuid, q.card);
    auto r    = rate(*s);
    auto base = FeePolicy::convert(q.local, r);
    limits(*s, cfg, q.xuid, Direction::Withdraw, base);

    auto lots = s->lots(c.no);
    verify(c, lots);

    auto plan  = withdrawPlan(lots, base, node.id, cfg.cross);
    auto total = plus(base, plan.fee);
    require(c.balance >= total, "BALANCE", "银行卡余额不足");

    auto legacy = money.get(q.xuid);
    require(legacy && *legacy >= 0, "LEGACY", "原版经济不可用");
    fits(*legacy, q.local);

    Intent i{};
    i.id         = ids.next();
    i.xuid       = q.xuid;
    i.card       = q.card;
    i.dir        = Direction::Withdraw;
    i.state      = State::PendingLegacy;
    i.server     = node.id;
    i.base       = base;
    i.fee        = plan.fee;
    i.local      = q.local;
    i.rate       = r;
    i.requestKey = q.key;
    i.created    = clock.now();
    i.updated    = i.created;
    i.slices     = plan.principal;
    i.slices.insert(i.slices.end(), plan.burned.begin(), plan.burned.end());
    {
        Transaction tx(*s);
        s->insertIntent(i);
        require(s->cas(i.id, State::PendingLegacy, State::PendingDb, clock.now()), "TAKEN", "该操作已处理");
        for (auto const& slice : i.slices) {
            s->consumeLot(slice.lot, slice.amount);
        }
        c.balance -= total;
        ++c.version;
        s->saveCard(c);
        ledger(*s, i, c, -total, "WITHDRAW");
        tx.commit();
    }

    // 后加目标：原版到账失败时补偿回滚银行侧。
    auto outcome = money.add(q.xuid, q.local);
    if (outcome == MoneyOutcome::Failed) {
        undoWithdrawal(*s, i, State::PendingDb);

        throw Failure("LEGACY", "原版到账失败，已退回银行卡");
    }
    if (outcome == MoneyOutcome::Unknown) {
        throw Failure("PENDING", "到账结果待确认，单号 " + i.id);
    }

    {
        Transaction tx(*s);
        require(s->cas(i.id, State::PendingDb, State::Settled, clock.now()), "TAKEN", "该操作已处理");
        tx.commit();
    }

    return Result::success({
        {"intent_id",    i.id    },
        {"local_amount", q.local },
        {"fee_base",     plan.fee}
    });
}

Result Bank::pay(PayRequest const& q) {
    identity(q.xuid);
    require(q.base > 0, "AMOUNT", "金额必须为正整数");

    // 先定位收款卡所属玩家，再按排序后的 xuid 加锁重读，避免两把锁交叉死锁。
    std::string targetNo;
    std::string targetXuid;
    {
        auto lookup = db.open();
        auto card   = lookup->card(q.recipient);
        if (!card) {
            for (auto const& c : lookup->cards(q.recipient)) {
                if (c.slot == 0) {
                    card = c;
                }
            }
        }
        require(card && card->status != "CLOSED", "RECIPIENT", "对方未开通银行卡");

        targetNo   = card->no;
        targetXuid = card->xuid;
    }
    require(targetXuid != q.xuid, "SELF", "不能向自己转账");

    auto s = db.open({q.xuid, targetXuid});
    ready(*s, q.xuid);
    ready(*s, targetXuid);
    auto cfg = s->config();
    require(cfg.transfers, "DISABLED", "转账暂未开放");

    auto from = owned(*s, q.xuid, q.card);
    auto to   = owned(*s, targetXuid, targetNo);
    limits(*s, cfg, q.xuid, Direction::Transfer, q.base);

    auto fee = FeePolicy::calculate(q.base, cfg.transfer);
    require(fee < q.base, "FEE", "手续费超过本金");
    auto total = plus(q.base, fee);
    require(from.balance >= total, "BALANCE", "银行卡余额不足");
    fits(to.balance, q.base);

    auto lots = s->lots(from.no);
    verify(from, lots);
    verify(to, s->lots(to.no));

    auto principal = take(lots, q.base);
    auto burned    = take(lots, fee);

    Intent i{};
    i.id         = ids.next();
    i.xuid       = q.xuid;
    i.card       = q.card;
    i.dir        = Direction::Transfer;
    i.state      = State::PendingDb;
    i.server     = node.id;
    i.peer       = targetXuid;
    i.base       = q.base;
    i.fee        = fee;
    i.requestKey = q.key;
    i.created    = clock.now();
    i.updated    = i.created;
    i.slices     = principal;

    Transaction tx(*s);
    s->insertIntent(i);
    require(s->cas(i.id, State::PendingDb, State::Settled, clock.now()), "TAKEN", "该操作已处理");

    for (auto const& slice : principal) {
        s->consumeLot(slice.lot, slice.amount);
        Lot lot;
        lot.id       = ids.next();
        lot.card     = to.no;
        lot.origin   = slice.origin;
        lot.original = slice.amount;
        lot.remain   = slice.amount;
        lot.created  = slice.created;
        s->insertLot(lot);
    }
    for (auto const& slice : burned) {
        s->consumeLot(slice.lot, slice.amount);
    }

    from.balance -= total;
    ++from.version;
    to.balance += q.base;
    ++to.version;
    s->saveCard(from);
    s->saveCard(to);

    ledger(*s, i, from, -total, "TRANSFER_OUT", to.xuid);
    auto credit = i;
    credit.fee  = 0;
    ledger(*s, credit, to, q.base, "TRANSFER_IN", from.xuid);
    tx.commit();

    return Result::success({
        {"intent_id", i.id},
        {"fee_base",  fee }
    });
}

Result Bank::rates() {
    auto s    = db.open();
    Json data = Json::array();
    for (auto const& v : s->rates(clock.now())) {
        data.push_back({
            {"server_id",    v.server   },
            {"rate",         v.value    },
            {"effective_at", v.effective},
            {"set_by",       v.by       }
        });
    }

    return Result::success(std::move(data));
}

Result Bank::slots(std::string const& xuid) {
    identity(xuid);

    auto s = db.open();

    return Result::success({
        {"used",  s->cards(xuid).size()},
        {"limit", s->config().slots    }
    });
}

Result Bank::freeze(std::string const& no, bool frozen) {
    auto found = db.open()->card(no);
    require(found && found->status != "CLOSED", "CARD", "银行卡不存在");

    auto        s = db.open({found->xuid});
    Transaction tx(*s);
    auto        c = s->card(no);
    require(c && c->status != "CLOSED", "CARD", "银行卡不存在");

    c->status = frozen ? "FROZEN" : "ACTIVE";
    ++c->version;
    s->saveCard(*c);
    tx.commit();

    return Result::success({
        {"card_no", c->no    },
        {"status",  c->status}
    });
}

Result Bank::setRate(std::string const& server, Amount value, std::string const& by) {
    require(value > 0, "RATE", "汇率必须为正整数");

    auto        s     = db.open();
    auto        nodes = s->nodes();
    Transaction tx(*s);
    require(
        std::any_of(nodes.begin(), nodes.end(), [&](auto const& n) { return n.id == server; }),
        "NODE",
        "节点不存在"
    );
    s->appendRate({server, by, value, clock.now()});
    tx.commit();

    return Result::success({
        {"server_id", server},
        {"rate",      value }
    });
}

Result Bank::resolve(std::string const& id, bool sourceConfirmed, std::string const& by) {
    auto found = db.open()->intent(id);
    require(found && found->state == State::Orphan, "INTENT", "单据不是 ORPHAN");

    auto s = db.open({found->xuid});
    auto i = s->intent(id);
    require(i && i->state == State::Orphan, "INTENT", "单据状态已改变");

    auto note = "resolved by " + by;
    if (i->dir == Direction::Deposit && sourceConfirmed) {
        settleDeposit(*s, *i, State::Orphan, note);
    } else if (i->dir == Direction::Withdraw && !sourceConfirmed) {
        undoWithdrawal(*s, *i, State::Orphan, note);
    } else {
        Transaction tx(*s);
        auto        target = i->dir == Direction::Deposit ? State::RolledBack : State::Settled;
        require(s->cas(id, State::Orphan, target, clock.now(), note), "TAKEN", "该操作已处理");
        tx.commit();
    }

    return Result::success({
        {"intent_id", id}
    });
}

void Bank::recover(Time grace, Time offline) {
    auto scan    = db.open();
    auto pending = scan->pending(clock.now() - grace);
    auto nodes   = scan->nodes();

    for (auto const& candidate : pending) {
        try {
            auto        s       = db.open({candidate.xuid});
            auto        current = s->intent(candidate.id);
            auto const& i       = current ? *current : candidate;
            if (!current || current->updated > clock.now() - grace || current->state == State::Settled
                || current->state == State::RolledBack || current->state == State::Orphan) {
                continue;
            }

            auto owner = std::find_if(nodes.begin(), nodes.end(), [&](auto const& n) { return n.id == i.server; });
            auto local = i.server == node.id;
            if (local) {
                // 本服单据：按方向各自的安全序终结，绝不重放原版写入。
                if (i.state == State::PendingDb && i.dir == Direction::Deposit) {
                    settleDeposit(*s, i, State::PendingDb);
                } else if (i.state == State::PendingDb && i.dir == Direction::Withdraw) {
                    undoWithdrawal(*s, i, State::PendingDb);
                } else {
                    // PENDING_LEGACY：原版调用结果不明。存款放弃（少给方向），取款 DB 尚未动账。
                    Transaction tx(*s);
                    s->cas(i.id, i.state, State::RolledBack, clock.now(), "LegacyMoney outcome unconfirmed");
                    tx.commit();
                }
                continue;
            }

            // 归属节点失联：绝不代其重放，交人工处置。
            if (owner == nodes.end() || owner->heartbeat < clock.now() - offline) {
                Transaction tx(*s);
                s->cas(i.id, i.state, State::Orphan, clock.now(), "unconfirmed LegacyMoney call or offline owner");
                tx.commit();
            }
        } catch (...) {
            // 下一轮再试；任何异常都不得传播到原版调用方。
        }
    }
}

} // namespace rbank::core
