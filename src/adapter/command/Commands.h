#pragma once

#include "core/model/Model.h"

#include <memory>

class Player;

namespace rbank {
class Application;
}

namespace rbank::adapter {

void registerCommands(std::shared_ptr<Application> const&);
void showCards(std::shared_ptr<Application> const&, Player&);
void onJoin(std::shared_ptr<Application> const&, Player&);

/// 统一的文本呈现：指令输出、控制台日志与玩家消息共用。
std::string render(core::Result const&);

} // namespace rbank::adapter
