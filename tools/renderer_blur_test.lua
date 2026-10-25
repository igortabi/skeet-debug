-- In-game test for the renderer.blur fix (skeet-debug). Load it from the LUA tab like any script.
--
-- Controls in LUA > A:
--   "Blur test: callback"   off / paint_ui / paint: which callback calls renderer.blur
--   "Blur test: per frame"  how many renderer.blur calls each frame
-- One call blurs the framed box in the middle of the screen; the rest are 1 x 1 rects in the top-left corner, so
-- they fill the queue without covering the screen. The text in the top-left corner shows whether you are in game,
-- the calls this frame and the calls made while out of game (the ones that used to pile up).
--
-- Blur blurs everything the game drew (world, HUD, main menu, loading screen, team select, buy menu, console);
-- what the cheat draws (this text, the frame, the menu) stays sharp on top. It works in every game state with
-- paint_ui; paint does not run out of game.
--
-- Without the fix, paint_ui + the main menu fills the blur queue to 8192 rects in a few seconds and joining a map
-- crashes with "too many indices for index buffer", and the box is never blurred in the main menu.

local callback = ui.new_combobox("LUA", "A", "Blur test: callback", "off", "paint_ui", "paint")
local per_frame = ui.new_slider("LUA", "A", "Blur test: per frame", 1, 8000, 200)

local out_of_game_calls = 0
local calls_this_frame = 0

local function in_game()
    return globals.mapname() ~= nil
end

local function box()
    local screen_w, screen_h = client.screen_size()
    local w, h = 400, 250
    return math.floor((screen_w - w) / 2), math.floor((screen_h - h) / 2), w, h
end

local function blur_many()
    local count = ui.get(per_frame)
    renderer.blur(box())
    for _ = 2, count do
        renderer.blur(0, 0, 1, 1)
    end
    calls_this_frame = count
    if not in_game() then out_of_game_calls = out_of_game_calls + count end
end

local function status()
    local mode = ui.get(callback)
    local lines = {
        "blur test  callback: " .. mode .. "   per frame: " .. ui.get(per_frame),
        "in game: " .. tostring(in_game()) .. "   calls this frame: " .. calls_this_frame,
        "calls made out of game: " .. out_of_game_calls .. (out_of_game_calls >= 8192 and "   (old DLL: queue full, joining crashes)" or ""),
    }
    for i, line in ipairs(lines) do
        renderer.text(20, 20 + (i - 1) * 16, 255, 255, 255, 255, "", 0, line)
    end
    if mode ~= "off" then
        local x, y, w, h = box()
        renderer.rectangle(x - 2, y - 2, w + 4, 2, 255, 80, 80, 255)
        renderer.rectangle(x - 2, y + h, w + 4, 2, 255, 80, 80, 255)
        renderer.rectangle(x - 2, y, 2, h, 255, 80, 80, 255)
        renderer.rectangle(x + w, y, 2, h, 255, 80, 80, 255)
    end
end

client.set_event_callback("paint_ui", function()
    -- paint runs before paint_ui, so the count is shown here and reset after.
    if ui.get(callback) == "paint_ui" then blur_many() end
    status()
    calls_this_frame = 0
end)

client.set_event_callback("paint", function()
    if ui.get(callback) == "paint" then blur_many() end
end)
