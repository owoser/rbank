#pragma once

#include "core/port/Ports.h"

namespace rbank::infra {

struct SystemClock final : core::Clock {
    core::Time now() override;
};

/// 全局唯一 id：16 字节随机数，用作主键、幂等键与批次号。
struct RandomId final : core::IdGen {
    std::string next() override;
};

} // namespace rbank::infra
