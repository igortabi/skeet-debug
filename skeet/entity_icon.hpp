#pragma once

namespace skeetsdk::entity_icon
{
    // Scoreboard icons for players, drawn by panorama in the rank column (the icon render of Cheat_revealer.lua):
    //
    //   entity.set_icon(player, texture_id)  ->  true
    //       Shows the texture (an id from renderer.load_png / load_jpg / load_svg / load_rgba) next to the
    //       player's row on the scoreboard, in place of the rank icon. A string is taken as a panorama image path
    //       instead ("file://{images}/...", or a URL).
    //   entity.set_icon(player)  or  entity.set_icon(player, nil)
    //       Removes it, and the rank icon comes back.
    //   skeetsdk.set_icon(player, texture_id)  ->  true, or nil and a message
    //       The same without raising.
    //
    // Panorama shows images from files, not the cheat's textures, so renderer.load_* are wrapped to remember what
    // each texture was made from, and the first set_icon with it writes that to
    // csgo/materials/panorama/images/icons/skeetsdk/<hash of the bytes>.<png|jpg|svg> (load_rgba is encoded as PNG).
    // Only textures loaded after the wrapper was installed (before the first script runs) can be used.
    // Icons stay until removed, the local player joins another server, another player takes the entity index, or
    // the scripts are reloaded.
    //
    // Adds entity.set_icon / skeetsdk.set_icon (and the internal skeetsdk._icon_write / _icon_png) the first time a
    // script loads after `entity`, `renderer` and `panorama` exist. Meant to be called for every script load (the
    // Lua guard does); idempotent and cheap.
    void RegisterLuaApi(void* lua_state) noexcept;
}
