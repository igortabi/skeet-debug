-- In-game test for ui.new_texture (skeet-debug). Load it from the LUA tab like any script.
--
-- The automatic checks print to the console (ok / FAIL lines and a summary). Then look at the menu:
--   LUA > A    a green logo, a checker picture, a spinner and a checkbox below them.
--              "Show logo" hides and shows the logo; the controls below move up and down.
--              "Picture" swaps the picture; "Picture size" resizes it and the checkbox below moves.
--              "Animate spinner" turns the spinner while the menu is open (ui.set from paint_ui).
--   LUA > B    a banner, and a banner far wider than the column: it is cut off at the column's edge.
--   IMG tab    (made with skeetsdk.new_tab) A: a tall image, so the container scrolls.
--              B: images and checkboxes taking turns.
-- Change the menu scale (MISC > Settings > DPI scale): the images grow with the other controls.
-- Reload and unload the script a few times: the images go away and come back with it, no crash.
-- No blank gaps should be left by the calls the checks expect to fail.

local passed, failed = 0, 0

local function check(ok, what)
    if ok then
        passed = passed + 1
        client.color_log(150, 200, 60, "[texture test] ok    " .. what)
    else
        failed = failed + 1
        client.color_log(255, 80, 80, "[texture test] FAIL  " .. what)
    end
end

-- The call must raise; the message goes to the log so it can be read.
local function raises(what, fn, ...)
    local ok, message = pcall(fn, ...)
    check(not ok, what .. (ok and "" or "  (" .. tostring(message) .. ")"))
end

-- --- textures made in code, so the test needs no files -------------------------------------------

-- pixel(x, y) returns r, g, b, a (0-255).
local function rgba(width, height, pixel)
    local rows = {}
    for y = 0, height - 1 do
        local row = {}
        for x = 0, width - 1 do
            row[#row + 1] = string.char(pixel(x, y))
        end
        rows[#rows + 1] = table.concat(row)
    end
    return renderer.load_rgba(table.concat(rows), width, height)
end

local logo = renderer.load_svg([[
<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
  <circle cx="32" cy="32" r="30" fill="#9fca2b"/>
  <circle cx="32" cy="32" r="18" fill="#1e1e1e"/>
  <rect x="26" y="8" width="12" height="48" fill="#9fca2b"/>
</svg>]], 64, 64)

local checker = rgba(32, 32, function(x, y)
    local light = (math.floor(x / 8) + math.floor(y / 8)) % 2 == 0
    local v = light and 230 or 50
    return v, v, v, 255
end)

local banner = rgba(200, 40, function(x, y)
    local t = x / 199
    return math.floor(255 * t), 120, math.floor(255 * (1 - t)), 255
end)

local tall = rgba(16, 64, function(x, y)
    local band = math.floor(y / 8) % 2 == 0
    return band and 220 or 60, band and 160 or 60, 40, 255
end)

-- Eight spinner frames: a ring with a hand at 45 degree steps.
local spinner_frames = {}
for i = 0, 7 do
    local angle = math.rad(i * 45 - 90)
    spinner_frames[#spinner_frames + 1] = renderer.load_svg(string.format([[
<svg xmlns="http://www.w3.org/2000/svg" width="48" height="48">
  <circle cx="24" cy="24" r="20" fill="none" stroke="#ffffff" stroke-width="3"/>
  <line x1="24" y1="24" x2="%.1f" y2="%.1f" stroke="#9fca2b" stroke-width="5" stroke-linecap="round"/>
</svg>]], 24 + 16 * math.cos(angle), 24 + 16 * math.sin(angle)), 48, 48)
end

check(logo ~= nil and checker ~= nil and banner ~= nil and tall ~= nil and spinner_frames[8] ~= nil,
    "renderer.load_svg / load_rgba gave texture ids")

-- --- the API is there -----------------------------------------------------------------------------

check(type(ui.new_texture) == "function", "ui.new_texture exists")
check(type(skeetsdk) == "table" and type(skeetsdk.new_texture) == "function", "skeetsdk.new_texture exists")
check(type(skeetsdk) == "table" and type(skeetsdk.set_texture) == "function", "skeetsdk.set_texture exists")

-- --- LUA > A: show / hide, swap, resize, animate -----------------------------------------------------

ui.new_label("LUA", "A", "ui.new_texture test")
local logo_item = ui.new_texture("LUA", "A", logo, 64, 64)
check(type(logo_item) == "number", "ui.new_texture returns a reference")

local show_logo = ui.new_checkbox("LUA", "A", "Show logo")
ui.set(show_logo, true)
check(ui.get(show_logo) == true, "ui.set / ui.get on a checkbox still go to the game")
ui.set_callback(show_logo, function(item)
    ui.set_visible(logo_item, ui.get(item))
end)

local pictures = {
    Checker = { checker, 32, 32 },
    Logo = { logo, 64, 64 },
    Banner = { banner, 200, 40 },
}
local picture_item = ui.new_texture("LUA", "A", checker, 32, 32)
local picture = ui.new_combobox("LUA", "A", "Picture", "Checker", "Logo", "Banner")
local picture_size = ui.new_slider("LUA", "A", "Picture size", 25, 300, 100, true, "%")

local function update_picture()
    local p = pictures[ui.get(picture)]
    local scale = ui.get(picture_size) / 100
    ui.set(picture_item, p[1], math.max(1, math.floor(p[2] * scale)), math.max(1, math.floor(p[3] * scale)))
end
ui.set_callback(picture, update_picture)
ui.set_callback(picture_size, update_picture)

local spinner_item = ui.new_texture("LUA", "A", spinner_frames[1], 48, 48)
local animate = ui.new_checkbox("LUA", "A", "Animate spinner")
ui.set(animate, true)
ui.new_checkbox("LUA", "A", "A control below the images")

local frame = 1
local next_frame = 0
client.set_event_callback("paint_ui", function()
    if not ui.get(animate) or not ui.is_menu_open() then return end
    local now = globals.realtime()
    if now < next_frame then return end
    next_frame = now + 0.08
    frame = frame % #spinner_frames + 1
    ui.set(spinner_item, spinner_frames[frame])
end)

-- --- LUA > B: plain, and wider than the column ------------------------------------------------------

ui.new_label("LUA", "B", "Banner, then one wider than the column")
ui.new_texture("LUA", "B", banner, 200, 40)
ui.new_texture("LUA", "B", banner, 600, 30)
ui.new_checkbox("LUA", "B", "Below the wide banner")

-- --- a script-made tab: scrolling and mixed controls ------------------------------------------------

if type(skeetsdk.new_tab) == "function" and skeetsdk.new_tab("IMG") ~= nil then
    ui.new_label("IMG", "A", "A tall image: this container scrolls")
    ui.new_texture("IMG", "A", tall, 120, 700)
    ui.new_checkbox("IMG", "A", "At the very bottom")
    for i = 1, 6 do
        ui.new_texture("IMG", "B", i % 2 == 0 and logo or checker, 32 + i * 8, 32 + i * 8)
        ui.new_checkbox("IMG", "B", "Checkbox " .. i)
    end
else
    check(false, "skeetsdk.new_tab made the IMG tab")
end

-- --- things that must fail cleanly --------------------------------------------------------------------

raises("texture id must be a number", ui.new_texture, "LUA", "B", "logo", 10, 10)
raises("texture id must not be negative", ui.new_texture, "LUA", "B", -1, 10, 10)
raises("width must be whole", ui.new_texture, "LUA", "B", logo, 10.5, 10)
raises("width must be at least 1", ui.new_texture, "LUA", "B", logo, 0, 10)
raises("height must be at most 4096", ui.new_texture, "LUA", "B", logo, 10, 4097)
raises("width and height are required", ui.new_texture, "LUA", "B", logo)
raises("an unknown tab raises", ui.new_texture, "NOPE", "A", logo, 10, 10)
raises("an unknown container raises", ui.new_texture, "LUA", "NOPE", logo, 10, 10)
raises("ui.set on a texture wants a texture id", ui.set, picture_item, "x")
raises("ui.set on a texture wants width and height together", ui.set, picture_item, checker, 32)

local item, message = skeetsdk.new_texture("LUA", "B", logo, 0, 10)
check(item == nil and type(message) == "string", "skeetsdk.new_texture returns nil, message instead of raising")
local ok, set_message = skeetsdk.set_texture(show_logo, logo)
check(ok == nil and type(set_message) == "string", "skeetsdk.set_texture refuses a checkbox")
check(skeetsdk.set_texture(logo_item, logo) == true, "skeetsdk.set_texture works on a texture")
check(pcall(ui.set, logo_item, logo, 64, 64), "ui.set with a size works on a texture")

client.color_log(failed == 0 and 150 or 255, failed == 0 and 200 or 80, failed == 0 and 60 or 80,
    string.format("[texture test] %d passed, %d failed. Now check the menu (see the top of the script).", passed, failed))
