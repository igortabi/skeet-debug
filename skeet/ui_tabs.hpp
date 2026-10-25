#pragma once

namespace skeetsdk::ui_tabs
{
    // Adds two functions to the Lua global (and package.loaded) table `skeetsdk`:
    //
    //   skeetsdk.new_tab(name [, icon [, columns [, position]]])
    //       Creates a menu tab with two columns named "A" and "B" laid out like the built-in LUA
    //       tab, so ui.new_checkbox(name, "A", ...) works right away.
    //       icon:     texture id from renderer.load_* shown in the tab bar. Without one the bar
    //                 writes the tab's name instead, like the LUA tab does, so keep such names short
    //                 (a long name spills out of the bar). The bar draws the top-left 48 x 48 pixels
    //                 of the texture, centered, so load it at 48 x 48.
    //       columns:  false gives an empty tab to lay out yourself with new_container.
    //       position: where the tab goes in the tab bar: a number from 1 (the top, above RAGE),
    //                 "first" (= 1) or "last" (default; a number past the end means last). The tabs
    //                 from that spot down, built-in ones included, move one icon lower. Only the
    //                 icons move: tab numbers and the order of the "previous tab" key stay as created.
    //       A boolean as the 2nd argument is read as `columns`, as before `icon` existed.
    //       Returns true if the tab was created, false if a tab with that name already existed,
    //       or nil and a message if the arguments are bad or the menu is not ready.
    //
    //   skeetsdk.new_container(tab, name [, x, width, y, height])
    //       Adds a container to an existing tab. The tab is 24 x 24 blocks; x/y are the offset and
    //       width/height the size, in blocks (defaults: 0, 24, 0, 24 = the whole tab).
    //       Returns true / false (already existed) / nil, message like new_tab.
    //
    //   skeetsdk.set_tab_visible(tab, visible)
    //   skeetsdk.set_container_visible(tab, container, visible)
    //       Hide or show a tab (its icon leaves the tab bar and the icons below it close the gap) or one
    //       container of a tab, by name. Works on built-in tabs and containers too; the LUA tab cannot be
    //       hidden. Hiding the open tab goes to the LUA tab first. Return true, or nil and a message.
    //       Everything a script hid comes back when the Lua state is closed (load, unload or reload of any
    //       script), before the script runs again, like the tabs it made.
    //
    //   ui.set_visible("TAB", visible)  and  ui.set_visible("TAB", "CONTAINER", visible)
    //       The same two, through the game's ui.set_visible, which only takes control references (numbers).
    //       Anything else is passed to the game's function unchanged. Unknown names raise a Lua error,
    //       like the other ui.* functions. Installed in Lua (ui_wrapper_source) the first time a script loads
    //       after `ui` exists.
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
