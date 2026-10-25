#pragma once

namespace skeetsdk::cheat_revealer
{
    // Cheat_revealer.lua, built in: tells which cheat other players use from the voice packets their cheats send
    // (shared ESP and the like), and shows it as an ESP flag, as an icon on the scoreboard and in the player list.
    //
    //   VISUALS > Player ESP:  "Cheat revealer", its display options (Scoreboard icon, Flag) and icon set
    //                          (Multicolored, Unicolored, Nado & Ryha, Alternative NL icon)
    //   PLAYERS > Adjustments: "Cheat: ..." for the player selected in the player list
    //
    // Scripts get what the script gave them: the "cheat_detected" event ({ player, cheat_id, cheat_long,
    // cheat_color }) and require("gamesense/cheat_revealer") with get_cheat(player), has_data(player) and
    // clear_data(player). cheat_color is { r, g, b, a } with an unpack() method instead of a gamesense/color object.
    //
    // The detection is the script's, unchanged. The icons are embedded into skeet.dll (revealer_icons.rc) instead
    // of being downloaded, and are shown with entity.set_icon (entity_icon.hpp), so no workshop library is needed.
    //
    // It is a script of its own in every Lua state, installed by the first load after `client`, `ui`, `entity`,
    // `renderer` and skeetsdk.set_icon exist, and by lua_guard::KeepStateAlive when no script is loaded, so it
    // always runs. Adds the internal
    // skeetsdk._revealer_icon. Meant to be called for every script load (the Lua guard does), after
    // entity_icon::RegisterLuaApi; idempotent and cheap.
    void RegisterLuaApi(void* lua_state) noexcept;
}
