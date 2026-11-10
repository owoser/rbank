#pragma once

#include "ll/api/command/ParamTraits.h"

namespace rbank::adapter {

/// 保留整数原文：XUID 与 BIGINT 金额都不能经过 32 位解析器。
struct IntegerArgument : std::string {};

} // namespace rbank::adapter

namespace ll::command {

template <>
struct ParamTraits<rbank::adapter::IntegerArgument> : ParamTraitsBase<rbank::adapter::IntegerArgument> {
    static constexpr CommandRegistry::HardNonTerminal parseRuleValue() { return CommandRegistry::HardNonTerminal::Int; }

    struct Parser {
        bool operator()(
            CommandRegistry const&,
            void*                              storage,
            CommandRegistry::ParseToken const& token,
            CommandOrigin const&,
            int,
            std::string&,
            std::vector<std::string>&
        ) const {
            static_cast<rbank::adapter::IntegerArgument*>(storage)->assign(token.toString());

            return true;
        }
    };
};

} // namespace ll::command
