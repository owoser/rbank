#include "bootstrap/config/Config.h"

#include <fstream>

#include <nlohmann/json.hpp>

namespace rbank::bootstrap {

using core::Json;

namespace {

Json feeJson(core::Fee f) {
    return {
        {"ppm",   f.ppm  },
        {"fixed", f.fixed}
    };
}

core::Fee readFee(Json const& j) {
    core::require(
        j.at("ppm").is_number_integer() && j.at("fixed").is_number_integer(),
        "CONFIG",
        "Fee values must be integers"
    );

    core::Fee f{j.at("ppm").get<core::Amount>(), j.at("fixed").get<core::Amount>()};
    core::require(f.ppm >= 0 && f.ppm <= 1'000'000 && f.fixed >= 0, "CONFIG", "Fee configuration is invalid");

    return f;
}

/// 默认图标全部取自原版材质，运维换成自己的贴图只需改 data。
Json iconDefaults() {
    auto icon = [](char const* path) {
        return Json{
            {"data", path  },
            {"type", "path"}
        };
    };

    return {
        {"cards",      icon("textures/ui/icon_best3")    },
        {"open",       icon("textures/ui/color_plus")    },
        {"deposit",    icon("textures/ui/backup_replace")},
        {"withdraw",   icon("textures/ui/share_apple")   },
        {"transfer",   icon("textures/ui/trade_icon")    },
        {"history",    icon("textures/ui/creative_icon") },
        {"close_card", icon("textures/ui/icon_trash")    },
        {"back",       icon("textures/ui/icon_import")   },
        {"close",      icon("textures/ui/crossout")      }
    };
}

Json defaults() {
    return {
        {"server_id",   "server-a"                                                                },
        {"server_name", "Server A"                                                                },
        {"currency",    "Coin"                                                                    },
        {"database",
         {{"host", "127.0.0.1"},
          {"port", 3306},
          {"user", "rbank"},
          {"password", ""},
          {"database", "rbank"},
          {"library", "libmariadb.dll"},
          {"pool", 4},
          {"timeout", 3},
          {"verify_certificate", true},
          {"ca_file", ""}}                                                                        },
        {"card_no",
         {{"length", 12},
          {"fields", {"SERVER_CODE:2", "RANDOM_ALNUM:8", "SEQ:2"}},
          {"alphabet", "23456789ABCDEFGHJKLMNPQRSTUVWXYZ"},
          {"server_code", "A1"},
          {"checksum", "NONE"}}                                                                   },
        {"http",        {{"enabled", false}, {"host", "127.0.0.1"}, {"port", 8089}, {"token", ""}}},
        {"icons",       iconDefaults()                                                            },
        {"workers",     4                                                                         },
        {"queue",       128                                                                       }
    };
}

bool identifier(std::string const& s) {
    return !s.empty() && s.size() <= 40
        && s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == std::string::npos;
}

} // namespace

Json hotJson(core::HotConfig const& c) {
    return {
        {"slots",           c.slots            },
        {"deposit_fee",     feeJson(c.deposit) },
        {"cross_fee",       feeJson(c.cross)   },
        {"transfer_fee",    feeJson(c.transfer)},
        {"max_single_base", c.maxSingle        },
        {"max_daily_base",  c.maxDaily         },
        {"enabled",         c.enabled          },
        {"deposits",        c.deposits         },
        {"withdrawals",     c.withdrawals      },
        {"transfers",       c.transfers        },
        {"join_form",       c.joinForm         }
    };
}

core::HotConfig hotConfig(Json const& j) {
    core::require(
        j.at("slots").is_number_integer() && j.at("max_single_base").is_number_integer()
            && j.at("max_daily_base").is_number_integer(),
        "CONFIG",
        "Slots and limits must be integers"
    );

    core::HotConfig c;
    c.slots       = j.at("slots");
    c.deposit     = readFee(j.at("deposit_fee"));
    c.cross       = readFee(j.at("cross_fee"));
    c.transfer    = readFee(j.at("transfer_fee"));
    c.maxSingle   = j.at("max_single_base");
    c.maxDaily    = j.at("max_daily_base");
    c.enabled     = j.at("enabled");
    c.deposits    = j.at("deposits");
    c.withdrawals = j.at("withdrawals");
    c.transfers   = j.at("transfers");
    c.joinForm    = j.at("join_form");
    core::require(
        c.slots > 0 && c.slots <= 100 && c.maxSingle > 0 && c.maxDaily >= c.maxSingle,
        "CONFIG",
        "Slot or limit configuration is invalid"
    );

    return c;
}

bool loadConfig(std::filesystem::path const& path, StartupConfig& c) {
    if (!std::filesystem::exists(path)) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream templateFile(path);
        templateFile << defaults().dump(2);

        return false;
    }

    Json j;
    {
        std::ifstream in(path);
        j = Json::parse(in);
    }

    auto const& d                = j.at("database");
    c.node.id                    = j.at("server_id");
    c.node.name                  = j.at("server_name");
    c.node.currency              = j.at("currency");
    c.database.host              = d.at("host");
    c.database.port              = d.at("port");
    c.database.user              = d.at("user");
    c.database.password          = d.at("password");
    c.database.database          = d.at("database");
    c.database.library           = d.value("library", "libmariadb.dll");
    c.database.pool              = d.value("pool", 4);
    c.database.timeout           = d.value("timeout", 3u);
    c.database.verifyCertificate = d.value("verify_certificate", true);
    c.database.caFile            = d.value("ca_file", "");

    auto const& card  = j.at("card_no");
    c.card.length     = card.at("length");
    c.card.fields     = card.at("fields").get<std::vector<std::string>>();
    c.card.alphabet   = card.value("alphabet", c.card.alphabet);
    c.card.serverCode = card.at("server_code");
    c.card.checksum   = card.value("checksum", "NONE");

    auto const& h  = j.at("http");
    c.http.enabled = h.at("enabled");
    c.http.host    = h.at("host");
    c.http.port    = h.at("port");
    c.http.token   = h.at("token");

    // 图标表可整体省略；缺哪个键就沿用默认图，不让少写一行就起不来。
    auto icons = j.value("icons", Json::object());
    auto read  = [&](char const* key, IconConfig& out) {
        auto const& at = icons.find(key);
        if (at == icons.end() || !at->is_object()) {
            return;
        }

        out.data = at->value("data", out.data);
        out.type = at->value("type", "path");
    };
    read("cards", c.icons.cards);
    read("open", c.icons.open);
    read("deposit", c.icons.deposit);
    read("withdraw", c.icons.withdraw);
    read("transfer", c.icons.transfer);
    read("history", c.icons.history);
    read("close_card", c.icons.closeCard);
    read("back", c.icons.back);
    read("close", c.icons.close);

    c.workers = j.value("workers", 4);
    c.queue   = j.value("queue", 128);

    core::require(
        identifier(c.node.id) && identifier(c.database.database),
        "CONFIG",
        "server_id and database may only contain letters, digits, underscores and hyphens"
    );
    core::require(
        !c.node.name.empty() && c.node.name.size() <= 100 && !c.node.currency.empty() && c.node.currency.size() <= 40,
        "CONFIG",
        "Node name or currency name is invalid"
    );
    core::require(
        c.database.pool > 0 && c.database.pool <= 32 && c.workers > 0 && c.workers <= 16 && c.queue > 0
            && c.queue <= 4096 && c.database.timeout > 0 && c.database.timeout <= 30,
        "CONFIG",
        "Worker pool or connection pool configuration is invalid"
    );
    core::require(
        !c.http.enabled || (c.http.token.size() >= 32 && c.http.port > 0 && c.http.port <= 65535),
        "CONFIG",
        "HTTP admin token must be at least 32 characters"
    );

    return true;
}

} // namespace rbank::bootstrap
