#pragma once

namespace skeetsdk::voice_message
{
    // C++ port of https://github.com/uwukson4800/voice_message (voice_message.lua): script data sent to other
    // players inside the audio of a voice packet. Scripts get the library's API as `skeetsdk.voice_message`
    // (and from require("voice_message"), unless a file of that name was loaded first):
    //
    //   local voice_message = skeetsdk.voice_message
    //   local unsubscribe = voice_message(function(buffer, msg) ... end)
    //       Called for every relayed voice packet that has voice data, players talking included, so check a
    //       magic number first. msg.client is the sender's player slot (msg.entity = msg.client + 1); also
    //       msg.xuid_low / xuid_high, audible_mask, proximity, caster, format, sequence_bytes, section_number,
    //       uncompressed_sample_offset and voice_data (the raw bytes).
    //   voice_message.send(function(buffer) ... end)          -> true, or false and a message
    //   voice_message.send("setup_command", function(buffer, ...) ... end)  -> stop function (sends every event)
    //   voice_message.stop("setup_command")
    //   voice_message.buffer()                                 -> an empty buffer
    //
    //   buffer: write_bits(value, n), write_coord(f), write_string(s), read_bits(n), read_coord(), read_string(),
    //           crypt(key), reset(), bytes_written(), is_overflow(). At most 4096 bytes.
    //
    // Install() hooks the client's handler of relayed voice packets (CClientState::SVCMsg_VoiceData in
    // engine.dll), which queues the ones with voice data; the Lua side drains the queue from net_update_end
    // (the library calls the callbacks inside the hook). engine.dll is outside the range the cheat CRC-checks,
    // so it can run after skeet_t::extra().
    bool Install() noexcept;
    void Remove() noexcept;
    bool IsInstalled() noexcept;

    // Adds skeetsdk.voice_message (and the internal skeetsdk._voice_poll / _voice_send) to the Lua global
    // `skeetsdk` the first time a script loads after `client` exists. Meant to be called for every script load
    // (the Lua guard does); idempotent and cheap.
    void RegisterLuaApi(void* lua_state) noexcept;
}
