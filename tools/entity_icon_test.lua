-- In-game test for entity.set_icon. Join a server with players, load the script and open the scoreboard (Tab):
-- every player gets a green square in the rank column, the local player a red one. The checkbox removes them.

local function square(r, g, b)
    local pixels = {}
    for i = 1, 16 * 16 do
        pixels[i] = string.char(r, g, b, 255)
    end
    return renderer.load_rgba(table.concat(pixels), 16, 16)
end

local green = square(149, 184, 12)
local red = square(240, 6, 87)

local enabled = ui.new_checkbox("LUA", "A", "Scoreboard icons")
ui.set(enabled, true)

local applied = {}

local function refresh()
    local on = ui.get(enabled)
    for player = 1, globals.maxplayers() do
        local wanted = on and entity.get_classname(player) == "CCSPlayer"
        if wanted and not applied[player] then
            entity.set_icon(player, player == entity.get_local_player() and red or green)
            applied[player] = true
        elseif not wanted and applied[player] then
            entity.set_icon(player)
            applied[player] = nil
        end
    end
end

ui.set_callback(enabled, refresh)

local last = 0
client.set_event_callback("paint", function()
    if globals.realtime() - last < 1 then
        return
    end
    last = globals.realtime()
    refresh()
end)

client.set_event_callback("player_connect_full", function()
    applied = {}
end)
