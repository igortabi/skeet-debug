#pragma once

namespace skeetsdk::ui_tabs
{
    // Adds two functions to the Lua global (and package.loaded) table `skeetsdk`:
    //
    //   skeetsdk.new_tab(name [, columns])
    //       Creates a menu tab with two columns named "A" and "B" laid out like the built-in LUA
    //       tab, so ui.new_checkbox(name, "A", ...) works right away. Pass false as `columns` to
    //       get an empty tab and lay it out yourself with new_container.
    //       Returns true if the tab was created, false if a tab with that name already existed,
    //       or nil and a message if the arguments are bad or the menu is not ready.
    //
    //   skeetsdk.new_container(tab, name [, x, width, y, height])
    //       Adds a container to an existing tab. The tab is 24 x 24 blocks; x/y are the offset and
    //       width/height the size, in blocks (defaults: 0, 24, 0, 24 = the whole tab).
    //       Returns true / false (already existed) / nil, message like new_tab.
    //
    // Names are matched exactly by ui.new_*, so use the same spelling there. Call these when the
    // script loads, not from a paint callback. A tab lives as long as the script that made it:
    // loading, unloading or reloading any script closes the Lua state and runs the remaining scripts
    // again, and the tabs made by new_tab are removed at that moment (Install() below) and made
    // again by the scripts that are still loaded.
    //
    // Meant to be called for every script load (the Lua guard does); it is idempotent and cheap.
    void RegisterLuaApi(void* lua_state) noexcept;

    // Gives Menu->Tabs room for more tabs. MUST run once while the menu is built and idle (saver.cpp
    // calls it from the menu-init hook), never from a click: a script is loaded by a click, the click
    // is dispatched from inside a loop over Menu->Tabs, and reallocating that vector there makes the
    // loop read freed memory (access violation in the menu mouse handler). Without this call
    // new_tab fails with an error message instead of crashing.
    // `menu` is the CMenu* (saver.cpp passes the SDK's pointer).
    bool ReserveTabSlots(void* menu) noexcept;

    // Installs the hook that removes the tabs made by new_tab when the Lua state is closed. Like
    // config_scripts::Install it patches code inside the CRC-checked range, so call it BEFORE
    // skeet_t::extra(). Without it tabs simply stay after their script is unloaded.
    bool Install() noexcept;
    void Remove() noexcept;
    bool IsInstalled() noexcept;
}
