this project was made with AI. its basic skeet crack from https://github.com/sdkmasteri/skeet/tree/master but with debug lib. only thing i didnt not add was debug.debug bc i think its too dangerous and useless ( i mean its dangerous bc it could help crack some luas but idc, if yall will want i can add it)

## Verifying a release

Release builds are reproducible: the same commit built with the same Visual Studio toolset and Windows SDK gives a byte-identical `skeet.dll`, no matter which folder you build in. So you don't have to trust the DLL from the releases page, you can check it:

1. Check out the commit or tag the release was built from.
2. Build `Release | x86` with Visual Studio 2026 (toolset v145) and the Windows SDK named in the release notes (10.0.26100.0 for v1.1.1). In Visual Studio set it in the project's Windows SDK Version, or from a developer prompt:

```
msbuild skeet.sln /p:Configuration=Release /p:Platform=x86 /p:WindowsTargetPlatformVersion=10.0.26100.0
```

3. Compare hashes:

```
certutil -hashfile Release\skeet.dll SHA256
```

The hash should match the one published with the release. If your toolset or Windows SDK version differs from the one used for the release (a newer SDK is picked by default), the hash can differ too.

What makes this work (see `Directory.Build.targets`): `/Brepro` replaces timestamps with content hashes, `/pathmap` (with `/experimental:deterministic`, which MSVC 14.51 needs for it to apply) and `/PDBALTPATH` keep your folder paths out of the binary, and `skCrypt` derives its keys from `__LINE__` instead of `__TIME__`.

## skeetsdk.build

Scripts can see which build they are running on:

| Field | Value |
|---|---|
| `skeetsdk.build.commit` | git commit the DLL was built from (`"unknown"` when built without git) |
| `skeetsdk.build.dirty` | `true` if tracked files had uncommitted changes |
| `skeetsdk.build.tag` | release tag on that commit, or `nil` |
| `skeetsdk.build.id` | the commit, plus `-dirty` for modified builds |

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

## The script list follows the folder

The Config tab used to list the script files only when you selected it, so a script you added, deleted or renamed showed up only after switching to another tab and back. Now the list updates by itself when a file in the game folder is added, removed or renamed: the DLL watches the folder and runs what selecting the tab runs (the tab's own refresh, vtable `0x4346F6DC` slot 13), so the loaded marks and the selected script's info stay right.

## Script-made tabs

```lua
skeetsdk.new_tab("MYTAB")                           -- a tab with two columns, "A" and "B"
-- skeetsdk.new_tab("MYTAB", renderer.load_svg(svg, 48, 48), true, "first")  -- with an icon, at the top of the tab bar
ui.new_checkbox("MYTAB", "A", "hello")
ui.new_slider("MYTAB", "B", "slider", 0, 100, 50)
```

| Function | |
|---|---|
| `skeetsdk.new_tab(name [, icon [, columns [, position]]])` | creates a tab. `icon` is a texture id from `renderer.load_*`; the tab bar draws its top-left 48 x 48 pixels, so load it at 48 x 48. Without an icon the bar writes the tab's name, like the LUA tab, so keep it short (a long name spills out of the bar). `columns = false` makes the tab empty. `position` is where it goes in the tab bar: a number from 1 (the very top, above RAGE), `"first"` or `"last"` (default); the tabs below it, built-in ones included, move down one slot. A boolean as the 2nd argument still counts as `columns`. Returns `true` if created, `false` if a tab with that name exists, or `nil, message` |
| `skeetsdk.new_container(tab, name [, x, width, y, height])` | adds a container to a tab made by `new_tab`. A tab is 24 x 24 blocks; defaults fill it |

Names are matched exactly by `ui.new_*`. Call these when the script loads. Loading, unloading or reloading any script closes the Lua state and runs the remaining scripts again, so a tab disappears with its script and is made again by the scripts that are still loaded. There is room for 7 script tabs.

## Showing and hiding tabs and containers

`ui.set_visible` takes a control's reference. Given names instead of a reference it now also shows and hides tabs and containers:

```lua
ui.set_visible("APP", false)                -- the tab: its icon leaves the tab bar, the ones below move up
ui.set_visible("APP", "SETTINGS", false)    -- one container of a tab
```

A page switcher, with the page containers on top of each other and a combobox choosing which one shows:

```lua
skeetsdk.new_tab("APP", nil, false)                           -- an empty tab
skeetsdk.new_container("APP", "NAV",      0, 24, 0, 5)
skeetsdk.new_container("APP", "HOME",     0, 24, 5, 19)
skeetsdk.new_container("APP", "SETTINGS", 0, 24, 5, 19)       -- the same place as HOME

local page = ui.new_combobox("APP", "NAV", "Page", "Home", "Settings")
ui.new_checkbox("APP", "HOME", "Something on the home page")
ui.new_slider("APP", "SETTINGS", "Something in settings", 0, 100, 50)

local function show_page()
    local current = ui.get(page)
    ui.set_visible("APP", "HOME", current == "Home")
    ui.set_visible("APP", "SETTINGS", current == "Settings")
end
ui.set_callback(page, show_page)
show_page()
```

| Function | |
|---|---|
| `skeetsdk.set_tab_visible(tab, visible)` | what `ui.set_visible("TAB", visible)` does. Returns `true`, or `nil, message` |
| `skeetsdk.set_container_visible(tab, container, visible)` | what `ui.set_visible("TAB", "CONTAINER", visible)` does. Returns `true`, or `nil, message` |

Through `ui.set_visible` an unknown name raises a Lua error, like the other `ui.*` functions; the `skeetsdk` functions return `nil, message` instead. A number as the first argument goes to the game's own `ui.set_visible`, unchanged, so controls work as before and a control hidden that way stays independent of its container.

- Built-in tabs and containers can be hidden too, except the LUA tab (it is where scripts are managed). Hiding the open tab switches to the LUA tab first.
- A hidden container is neither drawn nor clickable; the controls in it keep their values. Containers keep their place, so hiding one leaves its space empty.
- A hidden tab holds no slot in the tab bar. Showing it puts it back where it was among the visible tabs.
- Whatever a script hid comes back when the Lua state closes (loading, unloading or reloading any script), before the scripts that remain run again, like the tabs a script made.

## Images in the menu

```lua
local svg = [[<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64"><circle cx="32" cy="32" r="30" fill="#9fca2b"/></svg>]]
local logo = ui.new_texture("LUA", "A", renderer.load_svg(svg, 64, 64), 64, 64)
ui.new_checkbox("LUA", "A", "below the image")

-- ui.set(logo, renderer.load_png(readfile("banner.png"), 200, 80), 200, 80)  -- another image, another size
-- ui.set_visible(logo, false)
```

| Function | |
|---|---|
| `ui.new_texture(tab, container, texture_id, width, height)` | adds a control that draws the texture stretched to `width` x `height`, below the previous control and lined up with the other controls' text. `texture_id` comes from `renderer.load_png`, `load_jpg`, `load_svg` or `load_rgba`. `width` and `height` are pixels at 100% menu scale (whole numbers, 1 to 4096); like every control it grows with the menu's DPI scale. Returns a reference, or raises an error like `ui.new_label` |
| `ui.set(texture, texture_id [, width, height])` | draws another texture from now on, and resizes the control when `width` and `height` are given |
| `skeetsdk.new_texture(...)`, `skeetsdk.set_texture(...)` | the same two, returning `nil, message` instead of raising |

- The reference is an ordinary one: `ui.set_visible` hides the image, the container scrolls it and cuts off what is wider than the container, and the image goes away with its script.
- At a menu scale other than 100% the texture is stretched to the scaled size. Load an SVG at the size it is drawn at if it has to be sharp.

How it works (images): `ui.new_texture` makes a label with the game's own `ui.new_label` (named `"\n"`, which the game stores as an empty name) and gives that one label a copy of the label vtable whose `Render` draws the texture and whose destructor forgets it. The label's default size becomes the image's, so the menu lays it out and scales it like any other control. Nothing in the cheat's code is patched. `tools/ui_texture_harness.cpp` tests the control on a fake label and renderer without the game.

## Listboxes that fit their items

```lua
local players = ui.new_listbox("LUA", "A", "Players", { "alpha", "bravo", "charlie" }, true)
ui.new_listbox("LUA", "A", "Mode", "Legit", "Rage", "HvH", true)   -- the strings form takes it last

ui.update(players, { "alpha", "bravo", "charlie", "delta" })       -- one row taller
```

A `true` after the items makes the listbox as tall as its search box and its items, instead of the game's fixed 300 px: no scrollbar and no empty rows. An empty list keeps one row, and a long one makes the container scroll instead.

- `ui.update` fits it again, and the controls below move with it.
- It shows every item at any menu scale.
- Typing in the search box filters as before; the height stays.
- Without the `true` (or with `false`) the listbox is the game's, unchanged.

How it works (listboxes): the game's `ui.new_listbox` reads items from the 4th argument on and skips anything that is not a string, so the trailing `true` is passed to it unchanged. A Lua wrapper then sets the listbox's height to its rows and gives that listbox a copy of the listbox vtable whose scale handler runs the game's and fits the height again, because the game recomputes the height from the default one and rounding at a scale like 125% can lose a row. `ui.update` is wrapped to fit it again after the game sets the new items. Nothing in the cheat's code is patched. `tools/ui_listbox_harness.cpp` tests it on a fake listbox without the game; `tools/ui_listbox_test.lua` is the in-game test script.

## More than 1024 controls in a container

The game gives a container room for 1024 controls when it hands them to its click handler and its drawing: controls after the 1024th never react to the mouse (or are never drawn). Hidden controls count for clicks, so a script that makes many controls and switches them with `ui.set_visible` could end up with visible controls that do not respond. There is no limit now; nothing changes for scripts below it.

How it works: the two routines that fill those lists (0x433C8238 for input, every control; 0x433FC71A for drawing, the visible ones) are replaced by ones that use the caller's 1024-slot list as before and a buffer of their own past that, sorted with the game's own sort and comparators. `tools/ui_children_harness.cpp` tests them on a fake container without the game; `tools/ui_children_test.lua` is the in-game test script.

## Font Awesome icons

```lua
ui.new_checkbox("LUA", "A", skeetsdk.get_icon("car") .. "  Drive")
ui.new_checkbox("LUA", "A", skeetsdk.get_icon("regular/heart") .. "  Like")
```

| Function | |
|---|---|
| `skeetsdk.get_icon(name)` | returns the icon as a one-character string (or `nil, message`), to put in any text the menu draws: checkboxes, sliders, buttons, labels, tab names |

`name` is a [Font Awesome Free](https://fontawesome.com/search?ic=free) 7 icon name or alias (`"car"`, `"gear"`, `"home"`), case-insensitive, with an optional `fa-` in front. Put a style before it with a slash to choose it: `"solid/heart"`, `"regular/heart"`, `"brands/discord"`. Without one you get the icon's main style: solid, else brands, else regular. Asking for a style the icon does not have (`"regular/car"`) returns `nil` and says which styles exist. The 47 icons whose character is an ordinary letter, digit or `*` (`"a"`, `"0"`) are left out, so normal text can never turn into an icon.

How it works: the menu draws text with its own glyph cache on top of GDI. skeet.dll embeds the three Font Awesome fonts, registers them as private fonts of the game process, and redirects only the cheat's own glyph calls (not gdi32, not the game's text) so that an icon's code point is measured and drawn with the Font Awesome font instead of the menu font, at 90% of the text height. The result is a character in the Private Use Area; regular and brands icons that share a code point with a solid one get an unused code point of their own, which `get_icon` hands out. If something stops this from loading, `get_icon` returns `nil` and the reason, and the menu is left as it was.

The fonts are Font Awesome Free 7.3.1 and are embedded as released (`skeet/third_party/fontawesome`, with their `LICENSE.txt`). `tools/generate-fontawesome.ps1` regenerates `skeet/fontawesome_icons.inc` from a Font Awesome release; `tools/fontawesome_harness.cpp` tests the GDI side without the game. Font Awesome Free by @fontawesome - https://fontawesome.com - icons are CC BY 4.0, fonts SIL OFL 1.1, code MIT: https://fontawesome.com/license/free

## Voice messages

A C++ port of [uwukson4800/voice_message](https://github.com/uwukson4800/voice_message) (`voice_message.lua`): scripts send bytes to other players inside the audio of a voice packet. The API is the library's, as `skeetsdk.voice_message`, and `require("voice_message")` returns it too, so scripts written for the library run unchanged:

```lua
local voice_message = skeetsdk.voice_message

voice_message(function(buffer, msg)
    if buffer:read_bits(16) ~= 0x1337 then
        return
    end

    print(string.format(
        "received voice packet from %s | pct_tickcount: %d",
        entity.get_player_name(msg.entity), buffer:read_bits(32)
    ))
end)

-- You only get your own packets back with voice_loopback 1.
client.set_event_callback("setup_command", function()
    if globals.tickcount() % 64 ~= 0 then return end
    voice_message.send(function(buffer)
        buffer:write_bits(0x1337, 16)
        buffer:write_bits(globals.tickcount(), 32)
    end)
end)
```

| Function | |
|---|---|
| `voice_message(callback)` | calls `callback(buffer, msg)` for every relayed voice packet with voice data, players talking included, so check a magic number first. Returns a function that removes the callback. `msg.client` is the sender's player slot, `msg.entity` its entity index; also `xuid_low`, `xuid_high`, `audible_mask`, `proximity`, `caster`, `format`, `sequence_bytes`, `section_number`, `uncompressed_sample_offset` and `voice_data` (the raw bytes) |
| `voice_message.send(function(buffer) ... end)` | calls the function with an empty buffer and sends what it wrote. Returns `true`, or `false, message` (for example when not connected) |
| `voice_message.send(event, function(buffer, ...) ... end)` | does the same on every `event` (`client.set_event_callback`). Returns a function that stops it; `voice_message.stop(event)` does too |
| `voice_message.buffer()` | an empty buffer |
| `buffer:write_bits(value, bits)`, `buffer:read_bits(bits)` | least significant bit first |
| `buffer:write_coord(value)`, `buffer:read_coord()` | the engine's bit coordinate (14 integer bits, 5 fraction bits) |
| `buffer:write_string(text)`, `buffer:read_string()` | zero-terminated |
| `buffer:crypt(key)` | XORs the bytes with `key` |
| `buffer:reset()`, `buffer:bytes_written()`, `buffer:is_overflow()` | |

- A buffer holds up to 4096 bytes; writing or reading past the end sets `is_overflow()`, and an overflowed buffer is not sent.
- The server relays a packet only to the players who can hear you: your team, unless the server has all-talk (`sv_alltalk`, `sv_full_alltalk`, `sv_deadtalk`, ...), and only with `sv_voiceenable 1`.
- Packets are delivered on `net_update_end` (the library calls the callbacks inside the engine's handler), so callbacks run on the game's main thread like every other event. Every callback gets the same buffer, reset in between; one that errors is reported and the others still run.

How it works: `send` builds the game's own `CCLCMsg_VoiceData` with the bytes as its voice data and format `VOICEDATA_FORMAT_STEAM`, and hands it to the net channel. The library sends it on the net channel's voice stream; the port sends it the way the game's own voice sender does, because a client never sets that stream up and a message sent on it makes the engine drop the connection ("Buffer overflow in send net message"). A hook on the client's handler of relayed packets (`CClientState::SVCMsg_VoiceData` in engine.dll, found by signature) queues every packet for the scripts. `tools/voice_message_test.lua` is the in-game test script.

### Every voice packet: the `voice_packet` event

```lua
client.set_event_callback("voice_packet", function(e)
    print(entity.get_player_name(e.entity), e.xuid, e.sequence_bytes, e.section_number, #e.voice_data)
end)
```

Fires for every voice packet the server relays, with or without audio, on `net_update_end`. `unset_event_callback("voice_packet", f)` removes it.

| Field | |
|---|---|
| `entity`, `client` | the sender's entity index, and its player slot (`entity - 1`) |
| `xuid` | the packet's 64-bit `xuid` as a decimal string (a SteamID64 when it is one) |
| `xuid_low`, `xuid_high` | the same as two 32-bit numbers |
| `audible_mask`, `proximity`, `caster`, `format` | as the server sent them (`proximity` and `caster` are booleans) |
| `sequence_bytes`, `section_number`, `uncompressed_sample_offset` | as the server sent them |
| `has_bits` | which optional fields the packet carries (protobuf has-bits: 1 client, 2 proximity, 4 xuid, 8 audible_mask, 16 voice_data, 32 caster, 64 format, 128 sequence_bytes, 256 section_number, 512 uncompressed_sample_offset) |
| `voice_data` | the audio bytes, `""` when there are none |

This is what the gamesense `voice` event gives as a pointer, decoded, with no FFI needed. Packets wait at most a second for a reader, so a callback set later doesn't get older ones.

`tools/voice_packet_test.lua` prints every packet. Something has to send packets for it to print anything: other players talking or running cheats, or its own test packet, which comes back to you with `voice_loopback 1`.

## Scoreboard icons

`entity.set_icon` puts a picture next to a player's row on the scoreboard, in place of the rank icon (the icon render of Cheat_revealer.lua):

```lua
local texture = renderer.load_png(readfile("icon.png"), 21, 21)

entity.set_icon(player, texture)   -- shows it
entity.set_icon(player)            -- removes it, the rank icon comes back
```

| Function | |
|---|---|
| `entity.set_icon(player, texture_id)` | shows the texture (an id from `renderer.load_png` / `load_jpg` / `load_svg` / `load_rgba`) for the player's entity index. A string is taken as a panorama image path instead (`"file://{images}/..."` or a URL). Raises a Lua error on bad arguments |
| `entity.set_icon(player)` | removes it |
| `skeetsdk.set_icon(player, texture_id)` | the same, returning `nil, message` instead of raising |

- Icons are shown at the rank icon's size (21 px high). They stay until removed, the local player joins another server, another player takes the entity index, or the scripts are reloaded.
- Only textures loaded after the first script started can be used (all of a script's own textures are).

How it works: the scoreboard is panorama and shows images from files, not the cheat's textures. `renderer.load_*` are wrapped in Lua to remember what each texture was made from, and the first `set_icon` with it writes that to `csgo/materials/panorama/images/icons/skeetsdk/<hash>.<png|jpg|svg>` (`load_rgba` data is encoded as PNG); the name is a hash of the bytes, so panorama's image cache never shows an older picture. The panorama code finds the player's row by XUID, adds an image slot after the rank column and hides the rank icon, and applies again whenever the scoreboard opens or updates. `tools/entity_icon_test.lua` is the in-game test script.

## Cheat revealer

Cheat_revealer.lua is built in: it tells which cheat other players use from the voice packets their cheats send (shared ESP and the like) and shows it as an ESP flag, as an icon on the scoreboard and in the player list. Unload your own copy of the script, or both make the same controls.

- **VISUALS > Player ESP:** "Cheat revealer", with its display options (Scoreboard icon, Flag) and icon set (Multicolored, Unicolored, Nado & Ryha, Alternative NL icon).
- **PLAYERS > Adjustments:** "Cheat: ..." for the player selected in the player list.
- Detections are remembered by SteamID (`skeetsdk.persist("cheat_revealer")`), so loading or unloading scripts, rejoining or a map change doesn't make the revealer start over: a player gets their cheat back as soon as they are seen. `clear_data(player)` forgets it. Nothing is kept after the game closes.
- Scripts get what the script gave them: the `cheat_detected` event (`player`, `cheat_id`, `cheat_long`, `cheat_color`) and `require("gamesense/cheat_revealer")` with `get_cheat(player)`, `has_data(player)` and `clear_data(player)`. `cheat_color` is a `{ r, g, b, a }` table with an `unpack()` method instead of a `gamesense/color` object.

The detection is the script's, unchanged. It needs no workshop library: the controls are made with `ui.*`, the icons are shown with `entity.set_icon`, and the three icon sets (from [dave3x8/revealer-icons](https://github.com/dave3x8/revealer-icons), `skeet/third_party/revealer_icons`) are embedded into skeet.dll (`revealer_icons.rc`) instead of being downloaded. It works with no script loaded: the cheat only has a Lua state while scripts are loaded, so when there is none the DLL makes one every frame before `paint_ui`, with the cheat's own function for it, and installs the built-in Lua additions into it (`lua_guard::KeepStateAlive`).

## Storage that survives script reloads

Loading or unloading any script closes the whole Lua state, so every script loses its variables. `skeetsdk.persist` keeps values in skeet.dll instead, and they come back in the next state:

```lua
local data = skeetsdk.persist("my_script")   -- a store for that namespace

data.loads = (data.loads or 0) + 1           -- written at once
data.settings = { color = { 255, 0, 0 }, enabled = true }
data.loads = nil                             -- removed

skeetsdk.persist.keys("my_script")           -- { "settings", ... }, sorted
skeetsdk.persist.clear("my_script")          -- removes everything in it
```

- Namespaces and keys are strings of 1 to 256 bytes. Use your script's name as the namespace so scripts don't overwrite each other.
- Values are `nil`, booleans, numbers (`inf` and `nan` too), strings (any bytes) and tables of those, with string, number or boolean keys. Functions, userdata, cdata, threads, tables that contain themselves and tables nested more than 64 levels deep raise an error at the assignment.
- Reading gives a copy: after changing a nested table, assign it again (`data.settings = settings`) to keep the change.
- Everything together is limited to 64 MB. Values last until the game closes; nothing is written to disk.

`tools/persist_test.lua` is the in-game test script.

## The jit table

gamesense's `jit` table only had `on`, `off`, `flush`, `status` and `attach`: the cheat's `luaopen_jit` still sets up the JIT, but no longer pushes the version fields or registers `jit.opt`. They are back:

```lua
print(jit.version, jit.version_num, jit.os, jit.arch)   -- LuaJIT 2.1.0-beta3  20100  Windows  x86

jit.opt.start(3)                         -- optimization level 0 to 3
jit.opt.start("-fold", "nocse", "sink")  -- flags on (+name, name) or off (-name, noname)
jit.opt.start("hotloop=100", "maxtrace=2000")
jit.opt.start()                          -- default flags (-O3)
```

- `jit.opt.start` works like LuaJIT 2.1's on the engine's own settings, and raises an error for an option it doesn't know. The flags are `fold cse dce fwd dse narrow loop abc sink fuse`; the parameters `maxtrace maxrecord maxirconst maxside maxsnap minstitch hotloop hotexit tryside instunroll loopunroll callunroll recunroll sizemcode maxmcode`. `jit.status()` shows the result.
- `require("jit.opt")` returns `jit.opt`.
- The settings are shared by every script and stay until the game closes.
- `jit.util` is not included: it hands out any function's bytecode and constants, which is exactly what obfuscated scripts hide.
- `jit.attach` is there but always raises "vmevent API disabled": the cheat's LuaJIT is built without VM events, so there is nothing to attach to.

`tools/jit_test.lua` is the in-game test script.

## Configs as files

Configs are kept as files in `Counter-Strike Global Offensive\skeet\configs\`, one `<name>.cfg` per config, so you can back them up, share them or delete them like any file:

- **Saving or deleting a config in the menu** updates the folder within a frame.
- **Copying a `.cfg` in, editing or renaming one, or deleting one** updates the menu's config list right away.
- Characters a file name cannot have are written as `%XX` (a config named `a:b` is `a%3Ab.cfg`). Config keys are kept in `keys.txt` next to them.
- A file that isn't a config (it has to start with the config signature) is left alone and never deleted.
- The first time, the folder gets every config you already have. Your old config file is kept once as `csgo\cfg\<id>.cfg.skeetsdk-backup`.

How it works: the cheat keeps all configs in one file, `csgo\cfg\<id>.cfg` (its path is at `0x434798CC`), which it reads again for every load and rewrites for every save or delete: `0xAF`, then per config the name, the data and the key slot. Nothing in the cheat is patched: every frame the DLL compares that file and the folder with what it last saw, writes the side that is behind, and after rebuilding the cheat's file from the folder reloads the Config tab's list (`0x4335C747`, what the Save button runs).

## A separate DPI scale for ESP

MISC > Settings > **DPI scale** now scales only the menu (and what scripts draw in `paint_ui`). The ESP has its own setting right below it, **ESP DPI scale**: 100% (the default) to 200%, or "Same as menu" for the old behaviour. It is saved in configs and kept across script reloads. It applies to everything the cheat draws in game, including what scripts draw in `paint`.

How it works: the cheat's DPI scale (`0x4339F0AD`) is one float at `0x4346D490`, from the table at `0x434745CC` (100% to 200%). The cheat's frame (`0x433AD576`) records the in-game features in one loop (over `0x43475A9C`), before `paint_ui` and the menu, and hands them `round(scale × 350)`, worked out just before the loop; ESP sizes come from that and the global while recording. Text, though, gets its size from its font, picked when the recorded overlay is replayed on the render thread: the drawing backend keeps an unscaled set of 7 fonts and one built for the DPI scale. So the loop's first instruction (`0x433AD5FC`) and the first one after it on every path (`0x433AD626`) now call stubs that set the global and that value to the ESP scale and put them back, and record two commands of their own around the ESP's: the overlay is a stream of commands (`0x433AB0FA` records one: a header `type << 27 | next offset`, then its data), replayed by calling a handler per type from the table at `0x4346D690`. The handler in slot 0 is wrapped: for those two commands it swaps in a set of fonts made at the ESP scale with the cheat's own `create_font` (`0x4334E194`) and swaps the menu's back after them, on the render thread, in replay order; every other command of that type goes to the original handler. The swap holds the backend's font lock (an SRW lock at `+0xA8`) exclusively, as the cheat's own font rebuild does. Glyphs and text metrics come from the game thread while recording, not from the replay: the text recorder adds every character to its font (`0x43342F16`), and the ESP places its name above the box by the font's line height (`0x4334113A`, through `0x4336C370`). So the font-set pickers (`0x434172EC`, `0x4336C370`) and the font-scale getter (`0x4335BCB2`) hand the game thread the ESP's fonts while it records the ESP, and the text handler (slot 10) adds an ESP text's glyphs to the ESP's fonts before drawing it.

## For contributors: hooking game code

The cheat CRC-checks its own code (`0x4331E000`, size `0x105B12`) and `skeet_t::extra()` stores the result. A hook inside that range that is installed after `extra()` makes the check fail and the menu never shows, so `config_scripts`, `ui_tabs` and `fontawesome` (it redirects glyph calls inside the range) install before it (see `dllmain.cpp`). The Lua guard patches code outside the range and can go after.