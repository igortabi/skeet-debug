-- In-game test for voice messages. Load it on two clients on the same team of one server (or on one client with
-- voice_loopback 1, which sends your own packets back to you), then watch the console.

local voice_message = skeetsdk.voice_message
local magic = 0x1337

voice_message(function(buffer, msg)
    if buffer:read_bits(16) ~= magic then
        return
    end

    local tickcount = buffer:read_bits(32)
    local position = buffer:read_coord()
    local text = buffer:read_string()

    print(string.format(
        "received voice packet from %s | pct_tickcount: %d | coord: %.3f | text: %s",
        entity.get_player_name(msg.entity), tickcount, position, text
    ))
end)

-- One packet a second; sending needs a connection, so not from the script's top level.
client.set_event_callback("setup_command", function()
    if globals.tickcount() % 64 ~= 0 then
        return
    end

    local ok, message = voice_message.send(function(buffer)
        buffer:write_bits(magic, 16)
        buffer:write_bits(globals.tickcount(), 32)
        buffer:write_coord(-123.5)
        buffer:write_string("hello")
    end)
    if not ok then
        print("voice_message.send: " .. tostring(message))
    end
end)
