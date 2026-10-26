#pragma once

namespace skeetsdk::jit_lib
{
    // The cheat's luaopen_jit (0x434344E2) still runs jit_init and registers jit.on / off / flush / status /
    // attach, but it no longer pushes jit.version / version_num / os / arch and no longer registers jit.opt or
    // jit.util. This puts back:
    //
    //   jit.version, jit.version_num, jit.os, jit.arch
    //   jit.opt.start(...)   (and require("jit.opt"))
    //       Like LuaJIT 2.1's: no arguments resets the optimization flags to the default (-O3); "0" to "3" sets an
    //       optimization level; "fold", "+cse", "-dce", "nofwd", ... turn one flag on or off; "hotloop=56", ...
    //       set a parameter (maxtrace maxrecord maxirconst maxside maxsnap minstitch hotloop hotexit tryside
    //       instunroll loopunroll callunroll recunroll sizemcode maxmcode). Raises an error for anything else.
    //
    // jit.attach stays the cheat's: its LuaJIT is built without VM events (LUAJIT_DISABLE_VMEVENT), so it raises
    // "vmevent API disabled" and there is no engine code left that would send them.
    // jit.util is not put back: it hands out any function's bytecode and constants, which is what obfuscated
    // scripts hide.
    //
    // Works on the engine's own state: J->flags at global_State+0x1E4 and J->param[15] at +0x788 (jit_init
    // fills them there), and lj_dispatch_init_hotcount (0x4342FADA) after hotloop changes, as LuaJIT does.
    // Meant to be called for every script load (the Lua guard does); idempotent and cheap.
    void RegisterLuaApi(void* lua_state) noexcept;
}
