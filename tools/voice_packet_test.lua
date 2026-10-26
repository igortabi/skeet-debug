-- In-game test for the voice_packet event. Every relayed voice packet is printed, with or without audio;
-- packets of the same player and the same fields at most once a second.
--
-- Something has to send packets: other players talking or running cheats, or this script itself. With
-- voice_loopback 1 the server sends your own packets back to you, and "Send a test packet" sends one a second.

local last_print = {}

local function on_voice_packet(e)
    local key = e.entity .. ":" .. e.has_bits .. ":" .. #e.voice_data
    if globals.realtime() - (last_print[key] or 0) < 1 then
        return
    end
    last_print[key] = globals.realtime()

    print(string.format(
        "voice_packet: %s (%d) xuid %s | audible_mask %d proximity %s caster %s format %d | sequence_bytes %d section_number %d sample_offset %d | has_bits 0x%X | %d bytes",
        entity.get_player_name(e.entity), e.entity, e.xuid, e.audible_mask, tostring(e.proximity), tostring(e.caster),
        e.format, e.sequence_bytes, e.section_number, e.uncompressed_sample_offset, e.has_bits, #e.voice_data
    ))
end

client.set_event_callback("voice_packet", on_voice_packet)

local print_packets = ui.new_checkbox("LUA", "A", "Print voice packets")
ui.set(print_packets, true)
ui.set_callback(print_packets, function()
    if ui.get(print_packets) then
        client.set_event_callback("voice_packet", on_voice_packet)
    else
        client.unset_event_callback("voice_packet", on_voice_packet)
    end
end)

local send_packets = ui.new_checkbox("LUA", "A", "Send a test packet (voice_loopback 1)")
ui.set(send_packets, true)

local next_send = 0
client.set_event_callback("paint", function()
    if not ui.get(send_packets) or globals.realtime() < next_send or entity.get_local_player() == nil then
        return
    end
    next_send = globals.realtime() + 1

    local ok, message = skeetsdk.voice_message.send(function(buffer)
        buffer:write_string("voice_packet_test")
    end)
    if not ok then
        print("voice_packet_test: send failed: " .. tostring(message))
    end
end)
