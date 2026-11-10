#pragma once

#include "core/model/Model.h"

#include <charconv>
#include <functional>
#include <string>

class Player;

namespace rbank::adapter {

/// 统一文本呈现：code 只用于程序分支，玩家只看到 message 与数据行。
inline std::string render(core::Result const& result) {
    std::string text = result.message;
    if (result.data.is_object()) {
        for (auto const& [key, value] : result.data.items()) {
            text += "\n" + key + ": " + (value.is_string() ? value.get<std::string>() : value.dump());
        }
    } else if (result.data.is_array()) {
        for (auto const& item : result.data) {
            if (item.is_object()) {
                bool first = true;
                for (auto const& [key, value] : item.items()) {
                    text  += first ? "\n" : " · ";
                    text  += key + ": " + (value.is_string() ? value.get<std::string>() : value.dump());
                    first  = false;
                }
            } else {
                text += "\n" + item.dump();
            }
        }
    }

    return text;
}

/// 按 xuid 定位在线玩家；结果回到主线程后再发送，绝不跨线程保存 Player 引用。
void playerByXuid(std::string const& xuid, std::function<void(Player&)> const& fn);

/// 严格解析正整数，拒绝空串、负号、溢出与尾随字符。
inline core::Amount amount(std::string const& value) {
    core::Amount parsed{};
    auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);

    core::require(ec == std::errc{} && ptr == value.data() + value.size() && parsed > 0, "AMOUNT", "金额必须为正整数");

    return parsed;
}

} // namespace rbank::adapter
