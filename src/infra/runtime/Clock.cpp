#include "infra/runtime/Clock.h"

#include <array>
#include <chrono>
#include <mutex>
#include <random>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <bcrypt.h>

namespace rbank::infra {

core::Time SystemClock::now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string RandomId::next() {
    std::array<unsigned char, 16> bytes{};
    core::require(
        BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0,
        "RANDOM",
        "系统随机源不可用"
    );

    static constexpr char hex[] = "0123456789abcdef";
    std::string           id;
    for (auto byte : bytes) {
        id += hex[byte >> 4];
        id += hex[byte & 15];
    }

    return id;
}

} // namespace rbank::infra
