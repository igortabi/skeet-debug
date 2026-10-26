#pragma once

namespace skeetsdk::persist
{
    // Storage that survives script reloads. Loading or unloading any script closes the whole Lua state, so every
    // script loses its variables; values kept here are held by skeet.dll and come back in the next state:
    //
    //   local data = skeetsdk.persist("my_script")   -- a store for that namespace
    //   data.kills = (data.kills or 0) + 1           -- written at once
    //   data.kills = nil                             -- removed
    //   skeetsdk.persist.keys("my_script")           -- { "kills", ... } (strings, sorted)
    //   skeetsdk.persist.clear("my_script")
    //
    // Keys are strings. Values are nil, booleans, numbers (inf and nan too), strings and tables of those (keys
    // strings, numbers or booleans; no cycles, at most 64 levels deep). Functions, userdata, cdata and threads raise
    // an error. Reading gives a copy: change a nested table and assign it again to keep the change. Everything
    // together is limited to 64 MB. The values last until the game closes; nothing is written to disk.
    //
    // Adds skeetsdk.persist (and the internal skeetsdk._persist_get / _persist_set / _persist_keys /
    // _persist_clear). Meant to be called for every script load (the Lua guard does); idempotent and cheap.
    void RegisterLuaApi(void* lua_state) noexcept;
}
