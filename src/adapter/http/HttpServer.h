#pragma once

#include "bootstrap/config/Config.h"

#include <memory>

namespace rbank {
class Application;
}

namespace rbank::adapter {

/// 内嵌管理接口：只读查询 + 纯管理域写入，不开放玩家存取转。
/// 默认关闭；启用时必须配置至少 32 字符令牌。
class HttpServer {
    struct Impl;
    std::unique_ptr<Impl> impl;

public:
    HttpServer(Application&, bootstrap::HttpConfig);
    ~HttpServer();

    void stop();
};

} // namespace rbank::adapter
