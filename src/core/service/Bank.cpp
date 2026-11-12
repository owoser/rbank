#include "core/service/Bank.h"

namespace rbank::core {

namespace {

/// card.balance_base 是 lot 的冗余汇总，两者不一致说明资金账已损坏，必须停手。
void verify(Card const& c, std::vector<Lot> const& lots) {
    Amount sum = 0;
    for (auto const& lot : lots) {
        sum = plus(sum, lot.remain);
    }

    require(sum == c.balance, "INVARIANT", "Card lots do not match the card balance");
}

} // namespace

Card Bank::owned(Session& s, std::string const& xuid, std::string const& no, bool active) {
    auto c = s.card(no);
    require(c && c->xuid == xuid && c->status != "CLOSED", "CARD", "Card not found");
    require(!active || c->status == "ACTIVE", "FROZEN", "Card is frozen");

    return *c;
}

void Bank::ready(Session& s, std::string const& xuid) {
    identity(xuid);

    auto cfg = s.config();
    require(cfg.enabled, "MAINTENANCE", "Bank is under maintenance");

    auto nodes = s.nodes();
    auto self  = std::find_if(nodes.begin(), nodes.end(), [this](auto const& n) { return n.id == node.id; });
    require(
        self != nodes.end() && self->name == node.name && self->currency == node.currency && self->status == "ONLINE",
        "NODE",
        "Bank node identity mismatch; read-only"
    );
    require(!s.unsettled(xuid), "PENDING", "A pending transfer is still being confirmed, try again later");
}

Amount Bank::rate(Session& s) {
    for (auto const& r : s.rates(clock.now())) {
        if (r.server == node.id && r.value > 0) {
            return r.value;
        }
    }

    throw Failure("RATE", "Local rate is not configured");
}

void Bank::limits(Session& s, HotConfig const& cfg, std::string const& xuid, Direction dir, Amount base) {
    require(base > 0 && base <= cfg.maxSingle, "LIMIT", "Amount exceeds the per-transaction limit");

    auto today = clock.now() / 86400 * 86400;
    require(plus(s.daily(xuid, dir, today), base) <= cfg.maxDaily, "LIMIT", "Amount exceeds the daily limit");
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

    throw Failure("CARD_GENERATION", "Card number generation failed, contact an administrator");
}

Result Bank::close(std::string const& xuid, std::string const& no) {
    auto        s = db.open({xuid});
    Transaction tx(*s);
    ready(*s, xuid);

    auto c = owned(*s, xuid, no);
    require(c.balance == 0, "BALANCE", "Card balance must be zero before closing");
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
        return Result::fail("EMPTY", "No bank card opened yet");
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

Result Bank::cash(std::string const& xuid) {
    identity(xuid);

    auto amount = money.get(xuid);
    require(amount && *amount >= 0, "LEGACY", "Legacy economy unavailable");

    return Result::success({
        {"amount", *amount}
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

/// 预览与下单走同一套校验，确认框里显示的余额就是下单后卡上会有的余额；
/// 预览不写库，因此两次之间余额被他处改动时，下单仍会按最新状态重新校验。
Result Bank::quote(std::string const& xuid, std::string const& no, Amount local, Direction dir) {
    auto s   = db.open({xuid});
    auto cfg = s->config();
    ready(*s, xuid);
    require(
        dir == Direction::Deposit ? cfg.deposits : cfg.withdrawals,
        "DISABLED",
        dir == Direction::Deposit ? "Deposits are temporarily unavailable" : "Withdrawals are temporarily unavailable"
    );

    auto c    = owned(*s, xuid, no);
    auto r    = rate(*s);
    auto base = FeePolicy::convert(local, r);
    limits(*s, cfg, xuid, dir, base);

    auto lots = s->lots(c.no);
    verify(c, lots);

    auto wallet = money.get(xuid);
    require(wallet && *wallet >= 0, "LEGACY", "Legacy economy unavailable");

    Amount balance{};
    Amount cash{};
    if (dir == Direction::Deposit) {
        auto fee = FeePolicy::calculate(base, cfg.deposit);
        require(fee < base, "FEE", "Fee exceeds the principal");
        require(*wallet >= local, "LEGACY_BALANCE", "Insufficient local balance or economy unavailable");

        balance = plus(c.balance, base - fee);
        cash    = *wallet - local;
    } else {
        // 取款手续费取决于批次来源，必须复用 withdrawPlan，否则预览与实际扣款不一致。
        auto plan  = withdrawPlan(lots, base, node.id, cfg.cross);
        auto total = plus(base, plan.fee);
        require(c.balance >= total, "BALANCE", "Insufficient card balance");
        fits(*wallet, local);

        balance = c.balance - total;
        cash    = plus(*wallet, local);
    }

    return Result::success({
        {"balance_base", balance},
        {"wallet_local", cash   },
        {"rate",         r      }
    });
}

Result Bank::localRate() {
    auto s = db.open();

    return Result::success({
        {"rate", rate(*s)     },
        {"unit", node.currency}
    });
}

void Bank::settleDeposit(Session& s, Intent const& i, State from, std::string const& note) {
    Transaction tx(s);
    require(s.cas(i.id, from, State::Settled, clock.now(), note), "TAKEN", "This operation was already handled");

    auto c      = owned(s, i.xuid, i.card, false);
    auto amount = i.base - i.fee;
    require(amount > 0, "INVARIANT", "Deposit credit amount is invalid");
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
    require(cfg.deposits, "DISABLED", "Deposits are temporarily unavailable");

    auto c    = owned(*s, q.xuid, q.card);
    auto r    = rate(*s);
    auto base = FeePolicy::convert(q.local, r);
    limits(*s, cfg, q.xuid, Direction::Deposit, base);

    auto fee = FeePolicy::calculate(base, cfg.deposit);
    require(fee < base, "FEE", "Fee exceeds the principal");
    verify(c, s->lots(c.no));
    fits(c.balance, base - fee);

    auto balance = money.get(q.xuid);
    require(balance && *balance >= q.local, "LEGACY_BALANCE", "Insufficient local balance or economy unavailable");

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

        throw Failure("LEGACY", "LegacyMoney debit failed; deposit was not executed");
    }
    if (outcome == MoneyOutcome::Unknown) {
        throw Failure("PENDING", Message{"Debit result pending confirmation, intent {0}", {i.id}});
    }

    {
        Transaction tx(*s);
        require(
            s->cas(i.id, State::PendingLegacy, State::PendingDb, clock.now()),
            "TAKEN",
            "This operation was already handled"
        );
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
    require(s.cas(i.id, from, State::RolledBack, clock.now(), note), "TAKEN", "This operation was already handled");

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
    require(restored == plus(i.base, i.fee), "INVARIANT", "Rollback lots are incomplete");

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
    require(cfg.withdrawals, "DISABLED", "Withdrawals are temporarily unavailable");

    auto c    = owned(*s, q.xuid, q.card);
    auto r    = rate(*s);
    auto base = FeePolicy::convert(q.local, r);
    limits(*s, cfg, q.xuid, Direction::Withdraw, base);

    auto lots = s->lots(c.no);
    verify(c, lots);

    auto plan  = withdrawPlan(lots, base, node.id, cfg.cross);
    auto total = plus(base, plan.fee);
    require(c.balance >= total, "BALANCE", "Insufficient card balance");

    auto legacy = money.get(q.xuid);
    require(legacy && *legacy >= 0, "LEGACY", "Legacy economy unavailable");
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
        require(
            s->cas(i.id, State::PendingLegacy, State::PendingDb, clock.now()),
            "TAKEN",
            "This operation was already handled"
        );
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

        throw Failure("LEGACY", "LegacyMoney credit failed; funds returned to the card");
    }
    if (outcome == MoneyOutcome::Unknown) {
        throw Failure("PENDING", Message{"Credit result pending confirmation, intent {0}", {i.id}});
    }

    {
        Transaction tx(*s);
        require(
            s->cas(i.id, State::PendingDb, State::Settled, clock.now()),
            "TAKEN",
            "This operation was already handled"
        );
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
    require(q.base > 0, "AMOUNT", "Amount must be a positive integer");

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
        require(card && card->status != "CLOSED", "RECIPIENT", "Recipient has no bank card");

        targetNo   = card->no;
        targetXuid = card->xuid;
    }
    require(targetXuid != q.xuid, "SELF", "Cannot transfer to yourself");

    auto s = db.open({q.xuid, targetXuid});
    ready(*s, q.xuid);
    ready(*s, targetXuid);
    auto cfg = s->config();
    require(cfg.transfers, "DISABLED", "Transfers are temporarily unavailable");

    auto from = owned(*s, q.xuid, q.card);
    auto to   = owned(*s, targetXuid, targetNo);
    limits(*s, cfg, q.xuid, Direction::Transfer, q.base);

    auto fee = FeePolicy::calculate(q.base, cfg.transfer);
    require(fee < q.base, "FEE", "Fee exceeds the principal");
    auto total = plus(q.base, fee);
    require(from.balance >= total, "BALANCE", "Insufficient card balance");
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
    require(s->cas(i.id, State::PendingDb, State::Settled, clock.now()), "TAKEN", "This operation was already handled");

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
        {"intent_id",   i.id  },
        {"base_amount", q.base},
        {"fee_base",    fee   }
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
    require(found && found->status != "CLOSED", "CARD", "Card does not exist");

    auto        s = db.open({found->xuid});
    Transaction tx(*s);
    auto        c = s->card(no);
    require(c && c->status != "CLOSED", "CARD", "Card does not exist");

    c->status = frozen ? "FROZEN" : "ACTIVE";
    ++c->version;
    s->saveCard(*c);
    tx.commit();

    return Result::success({
        {"card_no", c->no    },
        {"status",  c->status}
    });
}

Result Bank::setSlots(int count, std::string const& by) {
    require(count >= 1 && count <= 100, "CONFIG", "Slot limit must be between 1 and 100");

    auto        s      = db.open();
    auto        config = s->config();
    Transaction tx(*s);
    config.slots = count;
    s->putConfig(config, by);
    tx.commit();

    return Result::success({
        {"limit", count}
    });
}

Result Bank::setBaseUnit(std::string const& unit, std::string const& by) {
    requireBaseUnit(unit);

    auto        s      = db.open();
    auto        config = s->config();
    Transaction tx(*s);
    config.baseUnit = unit;
    s->putConfig(config, by);
    tx.commit();

    return Result::success({
        {"base_unit", unit}
    });
}

Result Bank::setRate(std::string const& server, Amount value, std::string const& by) {
    require(value > 0, "RATE", "Rate must be a positive integer");

    auto        s     = db.open();
    auto        nodes = s->nodes();
    Transaction tx(*s);
    require(
        std::any_of(nodes.begin(), nodes.end(), [&](auto const& n) { return n.id == server; }),
        "NODE",
        "Node does not exist"
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
    require(found && found->state == State::Orphan, "INTENT", "Intent is not ORPHAN");

    auto s = db.open({found->xuid});
    auto i = s->intent(id);
    require(i && i->state == State::Orphan, "INTENT", "Intent state has changed");

    auto note = "resolved by " + by;
    if (i->dir == Direction::Deposit && sourceConfirmed) {
        settleDeposit(*s, *i, State::Orphan, note);
    } else if (i->dir == Direction::Withdraw && !sourceConfirmed) {
        undoWithdrawal(*s, *i, State::Orphan, note);
    } else {
        Transaction tx(*s);
        auto        target = i->dir == Direction::Deposit ? State::RolledBack : State::Settled;
        require(s->cas(id, State::Orphan, target, clock.now(), note), "TAKEN", "This operation was already handled");
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
