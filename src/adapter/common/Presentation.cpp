#include "adapter/common/Presentation.h"

#include "ll/api/service/Bedrock.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

namespace rbank::adapter {

void playerByXuid(std::string const& xuid, std::function<void(Player&)> const& fn) {
    auto level = ll::service::getLevel();
    if (!level) {
        return;
    }

    level->forEachPlayer([&](Player& player) {
        if (player.getXuid() != xuid) {
            return true;
        }

        fn(player);

        return false;
    });
}

} // namespace rbank::adapter
