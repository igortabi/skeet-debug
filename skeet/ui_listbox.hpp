#pragma once

namespace skeetsdk::ui_listbox
{
    // Listboxes as tall as their items:
    //
    //   ui.new_listbox(tab, container, name, items, true)
    //   ui.new_listbox(tab, container, name, "a", "b", "c", true)
    //       A `true` after the items makes the listbox exactly as tall as its search box and its items, instead of
    //       the game's fixed 300 px: no scrollbar and no empty rows. An empty list keeps one row. ui.update(listbox,
    //       items) fits it again, and the controls below move with it. Without the `true` nothing changes.
    //
    // The game's ui.new_listbox reads items from the 4th argument on and skips anything that is not a string, so
    // the trailing boolean is passed to it unchanged. The listbox then gets a copy of the listbox vtable whose
    // scale handler (slot 10) runs the game's and fits the height again, because the game recomputes it from the
    // default height and rounding at a scale like 125% can lose a row. Nothing in the cheat's code is patched.
    //
    // Adds the internal skeetsdk._fit_listbox to the Lua global `skeetsdk` and wraps ui.new_listbox / ui.update in
    // Lua the first time a script loads after `ui` exists. Meant to be called for every script load (the Lua guard
    // does); it is idempotent and cheap.
    void RegisterLuaApi(void* lua_state) noexcept;
}
