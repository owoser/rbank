#pragma once

#include "core/model/Model.h"

#include <filesystem>
#include <string>
#include <vector>

namespace rbank::bootstrap {

/// 卡号规则：字段、字母表与校验位全部来自启动配置，编译期不写死。
struct CardConfig {
    int                      length     = 12;
    std::vector<std::string> fields     = {"SERVER_CODE:2", "RANDOM_ALNUM:8", "SEQ:2"};
    std::string              alphabet   = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
    std::string              serverCode = "A1";
    std::string              checksum   = "NONE";
};

struct HttpConfig {
    bool        enabled = false;
    std::string host    = "127.0.0.1";
    int         port    = 8089;
    std::string token;
};

/// 按钮图标：值是原版材质路径或 URL，type 取 path / url，留空表示不画图标。
struct IconConfig {
    std::string data;
    std::string type = "path";
};

/// 每个菜单按钮一个键，运维改这里就能换图；键名与代码里的按钮一一对应。
struct IconTable {
    IconConfig cards;
    IconConfig open;
    IconConfig deposit;
    IconConfig withdraw;
    IconConfig transfer;
    IconConfig history;
    IconConfig closeCard;
    IconConfig back;
    IconConfig close;
};

/// config/config.json：只在启动时读取，修改需重启。
struct StartupConfig {
    core::Node node{"server-a", "Server A", "Coin"};
    struct {
        std::string host = "127.0.0.1", user = "rbank", password, database = "rbank", library = "libmariadb.dll",
                    caFile;
        unsigned port              = 3306;
        int      pool              = 4;
        unsigned timeout           = 3;
        bool     verifyCertificate = true;
    } database;
    CardConfig card;
    HttpConfig http;
    IconTable  icons;
    int        workers = 4;
    int        queue   = 128;
};

/// 缺配置时生成模板并以 false 返回，由调用方拒绝加载。
bool loadConfig(std::filesystem::path const& path, StartupConfig& out);

/// 热更配置的 JSON 编解码，配置表与 HTTP 管理接口共用。
core::Json      hotJson(core::HotConfig const&);
core::HotConfig hotConfig(core::Json const&);

} // namespace rbank::bootstrap
