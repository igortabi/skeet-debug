#pragma once

namespace skeetsdk::config_scripts
{
    // Saves the list of loaded Lua scripts inside every config saved with the Save button and
    // restores it when that config is loaded with the Load button. Configs saved before this was
    // installed carry no list and leave the loaded scripts alone.
    //
    // Install() must run BEFORE skeet_t::extra(): the hooks patch code inside 0x4331E000..0x43423B12,
    // the range whose CRC extra() -> recompile() bakes into the VM. Patching after that makes the
    // cheat's integrity check fail and the menu never shows up.
    bool Install() noexcept;
    void Remove() noexcept;
    bool IsInstalled() noexcept;
}
