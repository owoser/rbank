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

} // namespace rbank::adapter
