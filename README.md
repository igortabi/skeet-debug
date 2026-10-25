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

What makes this work (see `Directory.Build.targets`): `/Brepro` replaces timestamps with content hashes, `/pathmap` and `/PDBALTPATH` keep your folder paths out of the binary, and `skCrypt` derives its keys from `__LINE__` instead of `__TIME__`.

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

## For contributors: hooking game code

The cheat CRC-checks its own code (`0x4331E000`, size `0x105B12`) and `skeet_t::extra()` stores the result. A hook inside that range that is installed after `extra()` makes the check fail and the menu never shows, so `config_scripts`, `ui_tabs` and `fontawesome` (it redirects glyph calls inside the range) install before it (see `dllmain.cpp`). The Lua guard patches code outside the range and can go after.