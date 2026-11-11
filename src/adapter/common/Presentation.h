#pragma once

#include "core/model/Model.h"

#include "ll/api/i18n/I18n.h"

#include <charconv>
#include <functional>
#include <string>
#include <string_view>

class Player;

namespace rbank::adapter {

/// 按语言码取译文；缺失时回落到键本身（源码中的英文原文），再顺序替换 {0}… 占位符。
inline std::string translate(core::Message const& message, std::string_view locale) {
    std::string text(ll::i18n::getInstance().get(message.key, locale));
    for (size_t i = 0; i < message.args.size(); ++i) {
        auto token = "{" + std::to_string(i) + "}";
        auto at    = text.find(token);
        if (at != std::string::npos) {
            text.replace(at, token.size(), message.args[i]);
        }
    }

    return text;
}

/// 统一文本呈现：code 只用于程序分支，玩家只看到 message 与数据行。
inline std::string render(core::Result const& result, std::string_view locale = {}) {
    std::string text = translate(result.message, locale);
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

    core::require(
        ec == std::errc{} && ptr == value.data() + value.size() && parsed > 0,
        "AMOUNT",
        "Amount must be a positive integer"
    );

    return parsed;
}

} // namespace rbank::adapter
