#pragma once

#include <cstdint>

namespace skeetsdk::lua_guard
{
    inline constexpr std::uintptr_t default_load_address = 0x43433944u;

    bool Install(std::uintptr_t target = default_load_address) noexcept;
    void Remove() noexcept;
    bool IsInstalled() noexcept;
}
