#pragma once

#include <cstdint>

namespace skeetsdk::lua_guard
{
    inline constexpr std::uintptr_t default_load_address = 0x43433944u;

    bool Install(std::uintptr_t target = default_load_address) noexcept;
    void Remove() noexcept;
    bool IsInstalled() noexcept;

    // Makes the cheat's Lua state when there is none (no script loaded) and installs the built-in Lua additions
    // into it, so they work with no script loaded too. The cheat only has a state while scripts are loaded: it
    // closes it on every script load or unload and makes it again only for the scripts left. Call it often on the
    // game's thread, where the cheat runs Lua (renderer_blur calls it every frame right before paint_ui).
    void KeepStateAlive() noexcept;
}
