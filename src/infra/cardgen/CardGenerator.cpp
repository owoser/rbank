#include "infra/cardgen/CardGenerator.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <array>
#include <bcrypt.h>
#include <charconv>
#include <unordered_set>

namespace rbank::infra {

using namespace core;

namespace {

void randomBytes(unsigned char* out, size_t n) {
    require(
        BCryptGenRandom(nullptr, out, static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0,
        "RANDOM",
        "系统随机源不可用"
    );
}

/// 拒绝采样消除取模偏差，卡号字段分布保持均匀。
unsigned choose(unsigned range) {
    unsigned char value = 0;
    do {
        randomBytes(&value, 1);
    } while (value >= 256u - (256u % range));

    return value % range;
}

std::string xuidHash(std::string const& xuid, std::string const& alphabet, int size) {
    BCRYPT_ALG_HANDLE alg{};
    require(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "HASH", "SHA256 不可用");

    std::array<unsigned char, 32> hash{};
    auto                          status = BCryptHash(
        alg,
        nullptr,
        0,
        reinterpret_cast<PUCHAR>(const_cast<char*>(xuid.data())),
        static_cast<ULONG>(xuid.size()),
        hash.data(),
        static_cast<ULONG>(hash.size())
    );
    BCryptCloseAlgorithmProvider(alg, 0);
    require(status >= 0, "HASH", "SHA256 计算失败");

    std::string out;
    for (int i = 0; i < size; ++i) {
        out += alphabet[hash[static_cast<size_t>(i) % hash.size()] % alphabet.size()];
    }

    return out;
}

} // namespace

CardGenerator::CardGenerator(bootstrap::CardConfig c) : cfg(std::move(c)) {
    require(cfg.length >= 4 && cfg.length <= 64, "CONFIG", "卡号长度必须在 4–64 之间");

    std::unordered_set<char> unique(cfg.alphabet.begin(), cfg.alphabet.end());
    require(
        unique.size() == cfg.alphabet.size() && unique.size() >= 2 && cfg.alphabet.size() <= 62
            && cfg.alphabet.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz")
                   == std::string::npos,
        "CONFIG",
        "卡号 alphabet 必须是无重复的数字字母表"
    );
    require(cfg.checksum == "NONE" || cfg.checksum == "LUHN" || cfg.checksum == "LUHN10", "CONFIG", "不支持的校验规则");

    int total = cfg.checksum == "NONE" ? 0 : 1;
    for (auto const& spec : cfg.fields) {
        auto colon = spec.find(':');
        require(colon != std::string::npos, "CONFIG", "卡号字段需要 TYPE:length");

        auto type      = spec.substr(0, colon);
        int  size      = 0;
        auto [ptr, ec] = std::from_chars(spec.data() + colon + 1, spec.data() + spec.size(), size);
        require(
            ec == std::errc{} && ptr == spec.data() + spec.size() && size > 0 && size <= 64,
            "CONFIG",
            "卡号字段长度无效"
        );
        require(
            type == "SERVER_CODE" || type == "SEQ" || type == "XUID_HASH" || type == "RANDOM_UPPER"
                || type == "RANDOM_LOWER" || type == "RANDOM_DIGIT" || type == "RANDOM_ALNUM",
            "CONFIG",
            "不支持的卡号字段"
        );

        if (type == "SERVER_CODE") {
            require(cfg.serverCode.size() == static_cast<size_t>(size), "CONFIG", "server_code 长度与规则不符");
        }
        if (cfg.checksum == "LUHN10") {
            auto digits = [](std::string const& s) { return s.find_first_not_of("0123456789") == std::string::npos; };
            require(
                type == "RANDOM_DIGIT" || type == "SEQ" || (type == "SERVER_CODE" && digits(cfg.serverCode)),
                "CONFIG",
                "LUHN10 只接受数字卡号"
            );
        }

        fields.emplace_back(type, size);
        total += size;
    }

    require(total == cfg.length, "CONFIG", "卡号字段总长（含校验位）必须等于 length");
}

std::string CardGenerator::generate(std::string const& xuid, Session& s) {
    std::string out;
    for (auto const& [type, size] : fields) {
        if (type == "SERVER_CODE") {
            out += cfg.serverCode;
            continue;
        }

        if (type == "SEQ") {
            auto value = std::to_string(s.sequence("card:" + cfg.serverCode));
            if (value.size() > static_cast<size_t>(size)) {
                value = value.substr(value.size() - size);
            }
            out += std::string(size - value.size(), '0') + value;
            continue;
        }

        std::string alphabet = type == "RANDOM_UPPER" ? "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                             : type == "RANDOM_LOWER" ? "abcdefghijklmnopqrstuvwxyz"
                             : type == "RANDOM_DIGIT" ? "0123456789"
                                                      : cfg.alphabet;
        if (type == "XUID_HASH") {
            out += xuidHash(xuid, alphabet, size);
            continue;
        }

        for (int i = 0; i < size; ++i) {
            out += alphabet[choose(static_cast<unsigned>(alphabet.size()))];
        }
    }

    if (cfg.checksum == "NONE") {
        return out;
    }

    // 通用 Luhn：权重在字母表基数下交替 2/1。
    auto   alphabet = cfg.checksum == "LUHN10" ? std::string("0123456789") : cfg.alphabet;
    size_t sum      = 0;
    size_t factor   = 2;
    size_t base     = alphabet.size();
    for (auto it = out.rbegin(); it != out.rend(); ++it) {
        auto digit = alphabet.find(*it);
        require(digit != std::string::npos, "CONFIG", "卡号字段含校验字母表以外的字符");

        auto add  = factor * digit;
        sum      += add / base + add % base;
        factor    = factor == 2 ? 1 : 2;
    }
    out += alphabet[(base - sum % base) % base];

    return out;
}

} // namespace rbank::infra
