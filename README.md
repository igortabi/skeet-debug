this project was made with AI. its basic skeet crack from https://github.com/sdkmasteri/skeet/tree/master but with debug lib. this is the debug.debug branch: it is the main branch plus debug.debug (see below). the main branch leaves debug.debug out bc its dangerous ( it could help crack some luas ).

## debug.debug

`debug.debug()` opens a console window with a `lua_debug> ` prompt. Every line you type is run as Lua code in the environment of the script that called `debug.debug()`, and errors are printed instead of thrown. Type `cont` to close the console and continue. The game is frozen while the console is open, like the stock Lua version blocks on stdin.

## Verifying a release

Release builds are reproducible: the same commit built with the same Visual Studio toolset gives a byte-identical `skeet.dll`, no matter which folder you build in. So you don't have to trust the DLL from the releases page, you can check it:

1. Check out the commit or tag the release was built from.
2. Build `Release | x86` with Visual Studio 2026 (toolset v145).
3. Compare hashes:

```
certutil -hashfile Release\skeet.dll SHA256
```

The hash should match the one published with the release. If your toolset version differs from the one used for the release, the hash can differ too.

What makes this work (see `Directory.Build.targets`): `/Brepro` replaces timestamps with content hashes, `/pathmap` and `/PDBALTPATH` keep your folder paths out of the binary, and `skCrypt` derives its keys from `__LINE__` instead of `__TIME__`.

## skeetsdk.build

Scripts can see which build they are running on:

| Field | Value |
|---|---|
| `skeetsdk.build.commit` | git commit the DLL was built from (`"unknown"` when built without git) |
| `skeetsdk.build.dirty` | `true` if tracked files had uncommitted changes |
| `skeetsdk.build.tag` | release tag on that commit, or `nil` |
| `skeetsdk.build.id` | the commit, plus `-dirty` for modified builds, and always ending in `-debug.debug` on this branch |

A script that only wants to run on official releases:

```lua
local official = {
    -- ["<commit of a release>"] = true,
}

local build = skeetsdk and skeetsdk.build
if not build or build.dirty or not official[build.commit] then
    error("this script needs an official skeet-debug release")
end
```

This identifies honest builds, it does not prove anything: a modified build can report the same values. It stops people from running your scripts on random forks by accident, not on purpose.

## Configs remember their scripts

Saving a config with the Save button also stores which scripts are loaded. Loading it with the Load button then loads those scripts and unloads the others (scripts marked "load on startup" are left alone), and only after that applies the values, so a script's settings come back on the first load.

Configs saved without this carry no list and leave your scripts alone. The list sits at the end of the config, so clipboard exports and `config.export` do not carry it, and `config.load` called from a script does not change which scripts are loaded. Controls that share a name and type inside one script all read the same saved value (the game finds a value by the hash of the name), which is a limit of the config format.

## Script-made tabs

```lua
skeetsdk.new_tab("MYTAB")                           -- a tab with two columns, "A" and "B"
ui.new_checkbox("MYTAB", "A", "hello")
ui.new_slider("MYTAB", "B", "slider", 0, 100, 50)
```

| Function | |
|---|---|
| `skeetsdk.new_tab(name [, columns])` | creates a tab; `columns = false` makes it empty. Returns `true` if created, `false` if a tab with that name exists, or `nil, message` |
| `skeetsdk.new_container(tab, name [, x, width, y, height])` | adds a container to a tab made by `new_tab`. A tab is 24 x 24 blocks; defaults fill it |

Names are matched exactly by `ui.new_*`. Call these when the script loads. Loading, unloading or reloading any script closes the Lua state and runs the remaining scripts again, so a tab disappears with its script and is made again by the scripts that are still loaded. There is room for 7 script tabs.

## For contributors: hooking game code

The cheat CRC-checks its own code (`0x4331E000`, size `0x105B12`) and `skeet_t::extra()` stores the result. A hook inside that range that is installed after `extra()` makes the check fail and the menu never shows, so `config_scripts` and `ui_tabs` install before it (see `dllmain.cpp`). The Lua guard patches code outside the range and can go after.