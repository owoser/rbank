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
    core::require(j.at("ppm").is_number_integer() && j.at("fixed").is_number_integer(), "CONFIG", "手续费必须为整数");

    core::Fee f{j.at("ppm").get<core::Amount>(), j.at("fixed").get<core::Amount>()};
    core::require(f.ppm >= 0 && f.ppm <= 1'000'000 && f.fixed >= 0, "CONFIG", "手续费配置无效");

    return f;
}

Json defaults() {
    return {
        {"server_id",   "server-a"                                                                },
        {"server_name", "服务器 A"                                                                },
        {"currency",    "金币"                                                                    },
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
        "卡槽与限额必须为整数"
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
        "卡槽或限额配置无效"
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
    c.workers      = j.value("workers", 4);
    c.queue        = j.value("queue", 128);

    core::require(
        identifier(c.node.id) && identifier(c.database.database),
        "CONFIG",
        "server_id 与 database 只能使用字母、数字、下划线及短横线"
    );
    core::require(
        !c.node.name.empty() && c.node.name.size() <= 100 && !c.node.currency.empty() && c.node.currency.size() <= 40,
        "CONFIG",
        "节点名称或货币名称无效"
    );
    core::require(
        c.database.pool > 0 && c.database.pool <= 32 && c.workers > 0 && c.workers <= 16 && c.queue > 0
            && c.queue <= 4096 && c.database.timeout > 0 && c.database.timeout <= 30,
        "CONFIG",
        "线程池或连接池配置无效"
    );
    core::require(
        !c.http.enabled || (c.http.token.size() >= 32 && c.http.port > 0 && c.http.port <= 65535),
        "CONFIG",
        "HTTP 需要至少 32 字符的管理令牌"
    );

    return true;
}

} // namespace rbank::bootstrap
