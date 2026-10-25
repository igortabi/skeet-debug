#include "voice_message.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

// Port of https://github.com/uwukson4800/voice_message (voice_message.lua). The library does all of this from Lua
// with FFI; here the engine side is native and the buffer and API stay in Lua (client_wrapper_source below).
//
// Voice packets in engine.dll (all verified in the CS:GO engine.dll of Aug 2026):
//
// The client sends CNetMessagePB<10, CCLCMsg_VoiceData> (0x58 bytes, constructor 0x1019F550, scalar deleting
// destructor in vtable slot 0). Its protobuf sits at +8; CCLCMsg_VoiceData::set_data(const void*, size)
// (0x10225FD0, __thiscall, retn 8) copies the bytes into it. format at +0x18 of the protobuf, has-bits at +0x2C.
// The library sets format 0 (VOICEDATA_FORMAT_STEAM) and all the has-bits (63), and so does this.
//
// The client handles a relayed packet in CClientState::SVCMsg_VoiceData (0x102866A0, __thiscall(this, const
// CSVCMsg_VoiceData&), retn 4), which gets the protobuf: client (player slot) at +8, audible_mask at +0xC, xuid
// at +0x10, voice_data (std::string*, its size at +0x10) at +0x18, proximity at +0x1C, caster at +0x1D, format at
// +0x20, sequence_bytes at +0x24, section_number at +0x28, uncompressed_sample_offset at +0x2C.
//
// Sending goes through IVEngineClient::GetNetChannelInfo (slot 78, the client state's net channel) and
// CNetChan::SendNetMsg (slot 40, __thiscall(this, INetMessage&, bForceReliable, bVoice), retn 0Ch). The library
// passes bVoice true; this passes false, like the game's own sender (0x102288C0): the client never sets up the
// voice stream (CNetChan+0x78 stays NULL with -1 bits), so a message sent on it does not fit, and SendNetMsg then
// calls ConnectionCrashed ("Buffer overflow in send net message").

extern std::uint8_t* PatternScan(void* module, const char* signature);

namespace skeetsdk::voice_message
{
#if defined(_M_IX86)
    namespace
    {
        // Lua C API in the mapped image (same addresses the Lua guard uses).
        constexpr std::uintptr_t lua_push_cclosure_address = 0x43432F73u;
        constexpr std::uintptr_t lua_set_field_address = 0x43435B37u;
        constexpr std::uintptr_t lua_push_string_address = 0x434370E4u;
        constexpr std::uintptr_t lua_push_lstring_address = 0x43426891u;
        constexpr std::uintptr_t lua_push_number_address = 0x4343525Fu;
        constexpr std::uintptr_t lua_to_lstring_address = 0x43435597u;
        constexpr std::uintptr_t lua_set_top_address = 0x43426BDFu;
        constexpr std::uintptr_t lua_find_table_address = 0x43426632u;
        constexpr std::uintptr_t lua_type_address = 0x4343654Au;
        constexpr std::uintptr_t lua_get_field_address = 0x4342EBA2u;
        constexpr std::uintptr_t lua_load_buffer_address = 0x43426D2Fu;
        constexpr std::uintptr_t lua_pcall_address = 0x4342D9B9u;
        constexpr int lua_registry_index = -10000;
        constexpr int lua_type_string = 4;
        constexpr int lua_type_table = 5;
        constexpr int lua_type_function = 6;
        constexpr std::int32_t itype_false = -2;
        constexpr std::int32_t itype_true = -3;

        // push ebp / mov ebp, esp / and esp, -8 / mov eax, [convar vtable] / sub esp, 184h (the library's
        // signature is longer and finds the same function)
        constexpr char voice_handler_signature[] = "55 8B EC 83 E4 F8 A1 ? ? ? ? 81 EC 84 01 00 00";
        // The library's: the calls to the message constructor and to set_data in the game's own sender.
        constexpr char voice_init_call_signature[] = "E8 ? ? ? ? 56 8D 84 24 ? ? ? ? 50 8D 4C 24 28";
        constexpr char voice_set_call_signature[] = "E8 ? ? ? ? 83 4C 24 ? ? 83 7C 24";
        constexpr std::array<std::uint8_t, 6> handler_prologue{ 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8 };

        constexpr std::size_t engine_get_net_channel_slot = 78;
        constexpr std::size_t net_channel_send_slot = 40;
        constexpr std::size_t message_destroy_slot = 0;
        constexpr std::size_t message_size = 0x58;

        // CCLCMsg_VoiceData, from the start of the CNetMessagePB.
        constexpr std::size_t outgoing_protobuf = 8;
        constexpr std::size_t outgoing_format = 8 + 0x18;
        constexpr std::size_t outgoing_has_bits = 8 + 0x2C;
        constexpr std::uint32_t outgoing_all_fields = 63;
        constexpr std::int32_t voice_format_steam = 0;

        // CSVCMsg_VoiceData, the protobuf the handler gets.
        constexpr std::size_t incoming_client = 0x08;
        constexpr std::size_t incoming_audible_mask = 0x0C;
        constexpr std::size_t incoming_xuid = 0x10;
        constexpr std::size_t incoming_voice_data = 0x18;
        constexpr std::size_t incoming_proximity = 0x1C;
        constexpr std::size_t incoming_caster = 0x1D;
        constexpr std::size_t incoming_format = 0x20;
        constexpr std::size_t incoming_sequence_bytes = 0x24;
        constexpr std::size_t incoming_section_number = 0x28;
        constexpr std::size_t incoming_sample_offset = 0x2C;
        constexpr std::size_t string_size = 0x10;      // MSVC std::string
        constexpr std::size_t string_capacity = 0x14;

        constexpr std::size_t max_bytes = 4096;  // the library's buffer
        constexpr std::size_t max_queued = 256;
        constexpr ULONGLONG max_queued_age_ms = 1000;  // nobody polled them in time (no script listens)

        struct TValue
        {
            std::uint32_t value;
            std::int32_t type;
        };

        using LuaCFunction = int(__cdecl*)(void*);
        using PushCClosureFn = void(__cdecl*)(void*, LuaCFunction, int);
        using SetFieldFn = void(__cdecl*)(void*, int, const char*);
        using PushStringFn = void(__cdecl*)(void*, const char*);
        using PushLStringFn = void(__cdecl*)(void*, const char*, std::size_t);
        using PushNumberFn = void(__cdecl*)(void*, double);
        using ToLStringFn = const char*(__cdecl*)(void*, int, std::size_t*);
        using SetTopFn = void(__cdecl*)(void*, int);
        using FindTableFn = const char*(__cdecl*)(void*, int, const char*, int);
        using TypeFn = int(__cdecl*)(void*, int);
        using GetFieldFn = void(__cdecl*)(void*, int, const char*);
        using LoadBufferFn = int(__cdecl*)(void*, const char*, std::size_t, const char*);
        using PCallFn = int(__cdecl*)(void*, int, int, int);

        const auto push_cclosure = reinterpret_cast<PushCClosureFn>(lua_push_cclosure_address);
        const auto set_field = reinterpret_cast<SetFieldFn>(lua_set_field_address);
        const auto push_string = reinterpret_cast<PushStringFn>(lua_push_string_address);
        const auto push_lstring = reinterpret_cast<PushLStringFn>(lua_push_lstring_address);
        const auto push_number = reinterpret_cast<PushNumberFn>(lua_push_number_address);
        const auto to_lstring = reinterpret_cast<ToLStringFn>(lua_to_lstring_address);
        const auto set_top = reinterpret_cast<SetTopFn>(lua_set_top_address);
        const auto find_table = reinterpret_cast<FindTableFn>(lua_find_table_address);
        const auto get_type = reinterpret_cast<TypeFn>(lua_type_address);
        const auto get_field = reinterpret_cast<GetFieldFn>(lua_get_field_address);
        const auto load_buffer = reinterpret_cast<LoadBufferFn>(lua_load_buffer_address);
        const auto call_protected = reinterpret_cast<PCallFn>(lua_pcall_address);

        using HandlerFn = bool(__fastcall*)(void*, void*, const std::uint8_t*);  // __thiscall(this, msg), retn 4
        using CreateInterfaceFn = void*(__cdecl*)(const char*, int*);
        using ConstructFn = void*(__thiscall*)(void*);
        using SetDataFn = void(__thiscall*)(void*, const void*, std::size_t);
        using DestroyFn = void*(__thiscall*)(void*, int);
        using GetNetChannelFn = void*(__thiscall*)(void*);
        using SendNetMsgFn = bool(__thiscall*)(void*, void*, bool, bool);

        template <typename T>
        T At(const void* base, std::size_t offset) noexcept
        {
            T value;
            std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
            return value;
        }

        template <typename T>
        void Put(void* base, std::size_t offset, T value) noexcept
        {
            std::memcpy(static_cast<std::uint8_t*>(base) + offset, &value, sizeof(T));
        }

        template <typename Fn>
        Fn Virtual(void* object, std::size_t slot) noexcept
        {
            return reinterpret_cast<Fn>((*static_cast<void***>(object))[slot]);
        }

        // E8 rel32 -> the function it calls (the library's to_absolute).
        std::uint8_t* CallTarget(std::uint8_t* call) noexcept
        {
            return call == nullptr ? nullptr : call + 5 + At<std::int32_t>(call, 1);
        }

        struct Packet
        {
            ULONGLONG received;
            std::int32_t client;
            std::int32_t audible_mask;
            std::uint32_t xuid_low;
            std::uint32_t xuid_high;
            bool proximity;
            bool caster;
            std::int32_t format;
            std::int32_t sequence_bytes;
            std::uint32_t section_number;
            std::uint32_t sample_offset;
            std::string data;
        };

        struct HookState
        {
            std::mutex mutex;
            bool installed = false;
            std::uint8_t* target = nullptr;
            std::uint8_t* trampoline = nullptr;
            ConstructFn construct_message = nullptr;
            SetDataFn set_data = nullptr;
            void* engine_client = nullptr;
            const char* error = "voice messages are not installed";
        };

        HookState hook_state;
        HandlerFn original_handler = nullptr;

        // The handler runs on the main thread, and so do the scripts, but keep the queue safe anyway.
        SRWLOCK queue_lock = SRWLOCK_INIT;
        std::deque<Packet> queue;

        // The library hands every packet with voice data to the callbacks, players talking included.
        void Capture(const std::uint8_t* message) noexcept
        {
            const auto* voice_data = At<const std::uint8_t*>(message, incoming_voice_data);
            if (voice_data == nullptr) return;
            const auto size = At<std::uint32_t>(voice_data, string_size);
            if (size == 0) return;
            const auto* bytes = At<std::uint32_t>(voice_data, string_capacity) < 16
                ? voice_data : At<const std::uint8_t*>(voice_data, 0);
            AcquireSRWLockExclusive(&queue_lock);
            try
            {
                if (queue.size() >= max_queued) queue.pop_front();
                Packet& packet = queue.emplace_back();
                packet.received = GetTickCount64();
                packet.client = At<std::int32_t>(message, incoming_client);
                packet.audible_mask = At<std::int32_t>(message, incoming_audible_mask);
                packet.xuid_low = At<std::uint32_t>(message, incoming_xuid);
                packet.xuid_high = At<std::uint32_t>(message, incoming_xuid + 4);
                packet.proximity = At<std::uint8_t>(message, incoming_proximity) != 0;
                packet.caster = At<std::uint8_t>(message, incoming_caster) != 0;
                packet.format = At<std::int32_t>(message, incoming_format);
                packet.sequence_bytes = At<std::int32_t>(message, incoming_sequence_bytes);
                packet.section_number = At<std::uint32_t>(message, incoming_section_number);
                packet.sample_offset = At<std::uint32_t>(message, incoming_sample_offset);
                packet.data.assign(reinterpret_cast<const char*>(bytes), std::min<std::size_t>(size, max_bytes));
            }
            catch (...)
            {
            }
            ReleaseSRWLockExclusive(&queue_lock);
        }

        bool __fastcall OnVoiceData(void* client_state, void* edx, const std::uint8_t* message) noexcept
        {
            if (message != nullptr) Capture(message);
            return original_handler(client_state, edx, message);
        }

        // E9 rel32 written to `bytes`, to run at `at`: the offset is relative to where the jump runs, which is not
        // where the bytes are put together when they are copied over the game's code afterwards.
        void WriteJump(std::uint8_t* bytes, const std::uint8_t* at, const void* destination) noexcept
        {
            bytes[0] = 0xE9;
            *reinterpret_cast<std::uint32_t*>(bytes + 1) = static_cast<std::uint32_t>(
                reinterpret_cast<std::uintptr_t>(destination) - reinterpret_cast<std::uintptr_t>(at + 5));
        }

        bool WriteCode(std::uint8_t* target, const void* bytes, std::size_t size) noexcept
        {
            DWORD old_protection{};
            if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &old_protection)) return false;
            std::memcpy(target, bytes, size);
            DWORD ignored{};
            VirtualProtect(target, size, old_protection, &ignored);
            FlushInstructionCache(GetCurrentProcess(), target, size);
            return true;
        }

        // --- Lua ------------------------------------------------------------------------------------

        // A C function is guaranteed LUA_MINSTACK free slots. lua_State: top at +0x14.
        void PushBoolean(void* lua_state, bool value) noexcept
        {
            auto** top = reinterpret_cast<TValue**>(static_cast<std::uint8_t*>(lua_state) + 0x14);
            (*top)->value = 0;
            (*top)->type = value ? itype_true : itype_false;
            ++*top;
        }

        int Fail(void* lua_state, const char* message) noexcept
        {
            PushBoolean(lua_state, false);
            push_string(lua_state, message);
            return 2;
        }

        // skeetsdk._voice_poll() -> voice_data, client, audible_mask, xuid_low, xuid_high, proximity, caster,
        // format, sequence_bytes, section_number, uncompressed_sample_offset of the oldest queued packet, or nothing.
        int __cdecl NativePoll(void* lua_state) noexcept
        {
            Packet packet{};
            bool found = false;
            const ULONGLONG now = GetTickCount64();
            AcquireSRWLockExclusive(&queue_lock);
            while (!queue.empty())
            {
                packet = std::move(queue.front());
                queue.pop_front();
                if (now - packet.received <= max_queued_age_ms)
                {
                    found = true;
                    break;
                }
            }
            ReleaseSRWLockExclusive(&queue_lock);
            if (!found) return 0;
            push_lstring(lua_state, packet.data.data(), packet.data.size());
            push_number(lua_state, packet.client);
            push_number(lua_state, packet.audible_mask);
            push_number(lua_state, packet.xuid_low);
            push_number(lua_state, packet.xuid_high);
            PushBoolean(lua_state, packet.proximity);
            PushBoolean(lua_state, packet.caster);
            push_number(lua_state, packet.format);
            push_number(lua_state, packet.sequence_bytes);
            push_number(lua_state, packet.section_number);
            push_number(lua_state, packet.sample_offset);
            return 11;
        }

        // skeetsdk._voice_send(bytes) -> true, or false and a message (the library's send_raw)
        int __cdecl NativeSend(void* lua_state) noexcept
        {
            if (get_type(lua_state, 1) != lua_type_string) return Fail(lua_state, "bad voice data (string expected)");
            std::size_t size = 0;
            const char* bytes = to_lstring(lua_state, 1, &size);
            if (bytes == nullptr || size > max_bytes) return Fail(lua_state, "voice data is longer than 4096 bytes");

            ConstructFn construct = nullptr;
            SetDataFn set_data = nullptr;
            void* engine_client = nullptr;
            {
                std::scoped_lock lock(hook_state.mutex);
                if (!hook_state.installed) return Fail(lua_state, hook_state.error);
                construct = hook_state.construct_message;
                set_data = hook_state.set_data;
                engine_client = hook_state.engine_client;
            }
            void* net_channel = Virtual<GetNetChannelFn>(engine_client, engine_get_net_channel_slot)(engine_client);
            if (net_channel == nullptr) return Fail(lua_state, "not connected to a server");

            alignas(8) std::array<std::uint8_t, message_size> storage{};
            void* message = storage.data();
            construct(message);
            set_data(static_cast<std::uint8_t*>(message) + outgoing_protobuf, bytes, size);
            Put<std::int32_t>(message, outgoing_format, voice_format_steam);
            Put<std::uint32_t>(message, outgoing_has_bits, At<std::uint32_t>(message, outgoing_has_bits) | outgoing_all_fields);
            const bool sent = Virtual<SendNetMsgFn>(net_channel, net_channel_send_slot)(net_channel, message, false, false);
            Virtual<DestroyFn>(message, message_destroy_slot)(message, 0);  // frees the copy of the data
            if (!sent) return Fail(lua_state, "the net channel did not take the message");
            PushBoolean(lua_state, true);
            return 1;
        }

        // The library's Lua side: net_buffer (over a table of bytes instead of an FFI array), voice_message.send /
        // buffer / stop and voice_message(callback). Received packets are drained from net_update_end, which runs
        // right after the game reads the server's packets; every callback gets the same buffer, reset in between,
        // like the library does, and a callback that errors is reported and the others still run.
        constexpr char client_wrapper_source[] = R"LUA(
local sdk = skeetsdk
if type(sdk.voice_message) == "table" then return end
if type(client) ~= "table" or type(client.set_event_callback) ~= "function" or type(client.unset_event_callback) ~= "function" then return end
if type(bit) ~= "table" then return end
local poll, send = sdk._voice_poll, sdk._voice_send
local set_callback, unset_callback = client.set_event_callback, client.unset_event_callback
local error_log = type(client.error_log) == "function" and client.error_log or print
local band, bor, bxor, lshift, rshift = bit.band, bit.bor, bit.bxor, bit.lshift, bit.rshift
local floor, abs, type, tostring, error, pcall, setmetatable = math.floor, math.abs, type, tostring, error, pcall, setmetatable
local byte, char, concat, insert, remove, unpack = string.byte, string.char, table.concat, table.insert, table.remove, unpack

local max_bytes = 4096
local coord_integer_bits = 14
local coord_fractional_bits = 5
local coord_denominator = lshift(1, coord_fractional_bits)
local coord_resolution = 1.0 / coord_denominator

-- _data holds the bytes by index from 0; a byte that is not there is 0.
local net_buffer = {}
net_buffer.__index = net_buffer

function net_buffer.new()
    return setmetatable({ _data = {}, _cur_bit = 0, _read_bit = 0, _data_bits = 0, _overflow = false }, net_buffer)
end

function net_buffer.from_data(str)
    local data, size = {}, #str
    if size > max_bytes then size = max_bytes end
    for i = 1, size do data[i - 1] = byte(str, i) end
    return setmetatable({ _data = data, _cur_bit = 0, _read_bit = 0, _data_bits = size * 8, _overflow = false }, net_buffer)
end

function net_buffer:write_bits(val, num_bits)
    val = band(val, 0xFFFFFFFF)
    local data = self._data
    for i = 0, num_bits - 1 do
        if self._cur_bit >= max_bytes * 8 then self._overflow = true; return self end
        local byte_idx = rshift(self._cur_bit, 3)
        local bit_idx = band(self._cur_bit, 7)
        if band(rshift(val, i), 1) ~= 0 then
            data[byte_idx] = bor(data[byte_idx] or 0, lshift(1, bit_idx))
        end
        self._cur_bit = self._cur_bit + 1
    end
    return self
end

function net_buffer:write_coord(f_val)
    local abs_val = abs(f_val)
    local int_val = floor(abs_val)
    local fract_val = band(floor(abs_val * coord_denominator), coord_denominator - 1)

    self:write_bits(int_val ~= 0 and 1 or 0, 1)
    self:write_bits(fract_val ~= 0 and 1 or 0, 1)

    if int_val ~= 0 or fract_val ~= 0 then
        self:write_bits(f_val < 0 and 1 or 0, 1)
        if int_val ~= 0 then self:write_bits(int_val - 1, coord_integer_bits) end
        if fract_val ~= 0 then self:write_bits(fract_val, coord_fractional_bits) end
    end

    return self
end

function net_buffer:write_string(str)
    if type(str) ~= "string" then
        str = tostring(str or "")
    end

    for i = 1, #str do
        self:write_bits(byte(str, i), 8)
    end

    self:write_bits(0, 8)

    return self
end

function net_buffer:read_bits(num_bits)
    local result, data = 0, self._data
    for i = 0, num_bits - 1 do
        if self._read_bit >= self._data_bits then self._overflow = true; return result end
        local byte_idx = rshift(self._read_bit, 3)
        local bit_idx = band(self._read_bit, 7)
        result = bor(result, lshift(band(rshift(data[byte_idx] or 0, bit_idx), 1), i))
        self._read_bit = self._read_bit + 1
    end
    return result
end

function net_buffer:read_coord()
    local int_flag = self:read_bits(1)
    local fract_flag = self:read_bits(1)

    if int_flag == 0 and fract_flag == 0 then return 0.0 end

    local sign_bit = self:read_bits(1)
    local int_val = int_flag ~= 0 and self:read_bits(coord_integer_bits) + 1 or 0
    local fract_val = fract_flag ~= 0 and self:read_bits(coord_fractional_bits) or 0
    local value = int_val + fract_val * coord_resolution

    return sign_bit ~= 0 and -value or value
end

function net_buffer:read_string()
    local chars = {}

    while not self:is_overflow() do
        local char_byte = self:read_bits(8)

        if char_byte == 0 or self:is_overflow() then
            break
        end

        chars[#chars + 1] = char(char_byte)
    end

    return concat(chars)
end

function net_buffer:crypt(key)
    if not key or key == "" then return self end
    local key_len = #key
    local size = (self._cur_bit > 0) and rshift(self._cur_bit + 7, 3) or rshift(self._data_bits + 7, 3)
    local data = self._data

    for i = 0, size - 1 do
        local key_byte = byte(key, (i % key_len) + 1)
        data[i] = bxor(data[i] or 0, key_byte)
    end

    return self
end

function net_buffer:reset()
    self._read_bit = 0
    if self._cur_bit > 0 then self._data_bits = self._cur_bit end
    return self
end

function net_buffer:bytes_written() return rshift(self._cur_bit + 7, 3) end
function net_buffer:is_overflow() return self._overflow end

local function to_bytes(buf)
    local count, data, out = buf:bytes_written(), buf._data, {}
    for i = 0, count - 1 do out[i + 1] = data[i] or 0 end
    return count > 0 and char(unpack(out, 1, count)) or ""
end

local receive_callbacks, listening = {}, false

local function dispatch()
    while true do
        local data, client_index, audible_mask, xuid_low, xuid_high, proximity, caster, format, sequence_bytes, section_number, sample_offset = poll()
        if data == nil then return end
        local msg = {
            client = client_index, entity = client_index + 1, audible_mask = audible_mask,
            xuid_low = xuid_low, xuid_high = xuid_high, proximity = proximity, caster = caster, format = format,
            sequence_bytes = sequence_bytes, section_number = section_number,
            uncompressed_sample_offset = sample_offset, voice_data = data,
        }
        local buf = net_buffer.from_data(data)
        local current = {}
        for i = 1, #receive_callbacks do current[i] = receive_callbacks[i] end
        for i = 1, #current do
            buf:reset()
            local ok, message = pcall(current[i], buf, msg)
            if not ok then error_log("voice_message: " .. tostring(message)) end
        end
    end
end

local function update_listening()
    if #receive_callbacks > 0 and not listening then
        set_callback("net_update_end", dispatch)
        listening = true
    elseif #receive_callbacks == 0 and listening then
        unset_callback("net_update_end", dispatch)
        listening = false
    end
end

local voice_message = {}
local registered_events = {}

local function send_raw(buf)
    if buf:is_overflow() then
        return false, "the buffer overflowed (more than 4096 bytes)"
    end
    return send(to_bytes(buf))
end

function voice_message.send(event_or_callback, callback)
    if type(event_or_callback) == "function" then
        local buf = net_buffer.new()
        event_or_callback(buf)
        return send_raw(buf)
    end

    local event = event_or_callback
    if registered_events[event] then
        unset_callback(event, registered_events[event])
    end

    registered_events[event] = function(...)
        local buf = net_buffer.new()
        callback(buf, ...)
        send_raw(buf)
    end

    set_callback(event, registered_events[event])

    return function()
        if registered_events[event] then
            unset_callback(event, registered_events[event])
            registered_events[event] = nil
        end
    end
end

function voice_message.buffer()
    return net_buffer.new()
end

function voice_message.stop(event)
    if registered_events[event] then
        unset_callback(event, registered_events[event])
        registered_events[event] = nil
    end
end

setmetatable(voice_message, {
    __call = function(self, callback)
        if type(callback) ~= "function" then error("bad argument #1 to 'voice_message' (function expected)", 2) end
        insert(receive_callbacks, callback)
        update_listening()
        return function()
            for i, cb in ipairs(receive_callbacks) do
                if cb == callback then
                    remove(receive_callbacks, i)
                    break
                end
            end
            update_listening()
        end
    end
})

sdk.voice_message = voice_message
if type(package) == "table" and type(package.loaded) == "table" and package.loaded.voice_message == nil then
    package.loaded.voice_message = voice_message
end
)LUA";

        bool installing_wrapper = false;

        // Runs on every load until it has worked once in this Lua state (client may not exist yet on the first
        // loads). luaL_loadbuffer goes through the Lua guard's lua_loadx hook, which calls RegisterLuaApi again,
        // hence the flag.
        void InstallClientWrapper(void* lua_state) noexcept
        {
            if (installing_wrapper) return;
            if (find_table(lua_state, lua_registry_index, "_LOADED._G.skeetsdk", 4) != nullptr) return;
            get_field(lua_state, -1, "voice_message");
            const bool done = get_type(lua_state, -1) == lua_type_table;
            set_top(lua_state, -3);
            if (done) return;
            installing_wrapper = true;
            if (load_buffer(lua_state, client_wrapper_source, sizeof(client_wrapper_source) - 1, "=skeetsdk") == 0)
            {
                if (call_protected(lua_state, 0, 0, 0) != 0) set_top(lua_state, -2); // drop the error message
            }
            else
            {
                set_top(lua_state, -2);
            }
            installing_wrapper = false;
        }
    }

    bool Install() noexcept
    {
        std::scoped_lock lock(hook_state.mutex);
        if (hook_state.installed) return true;

        HMODULE engine = GetModuleHandleA("engine.dll");
        if (engine == nullptr)
        {
            hook_state.error = "engine.dll is not loaded";
            return false;
        }
        const auto create_interface = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(engine, "CreateInterface"));
        void* engine_client = create_interface != nullptr ? create_interface("VEngineClient014", nullptr) : nullptr;
        std::uint8_t* handler = PatternScan(engine, voice_handler_signature);
        std::uint8_t* constructor = CallTarget(PatternScan(engine, voice_init_call_signature));
        std::uint8_t* set_data = CallTarget(PatternScan(engine, voice_set_call_signature));
        if (engine_client == nullptr || handler == nullptr || constructor == nullptr || set_data == nullptr)
        {
            hook_state.error = "the game's voice code is not where it should be (game update?)";
            return false;
        }

        auto* trampoline = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, handler_prologue.size() + 5,
            MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        if (trampoline == nullptr)
        {
            hook_state.error = "out of memory";
            return false;
        }
        std::memcpy(trampoline, handler, handler_prologue.size());
        WriteJump(trampoline + handler_prologue.size(), trampoline + handler_prologue.size(), handler + handler_prologue.size());
        original_handler = reinterpret_cast<HandlerFn>(trampoline);

        std::array<std::uint8_t, handler_prologue.size()> patch{};
        WriteJump(patch.data(), handler, reinterpret_cast<const void*>(&OnVoiceData));
        std::memset(patch.data() + 5, 0x90, patch.size() - 5);
        if (!WriteCode(handler, patch.data(), patch.size()))
        {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            original_handler = nullptr;
            hook_state.error = "could not patch the game's voice handler";
            return false;
        }

        hook_state.target = handler;
        hook_state.trampoline = trampoline;
        hook_state.construct_message = reinterpret_cast<ConstructFn>(constructor);
        hook_state.set_data = reinterpret_cast<SetDataFn>(set_data);
        hook_state.engine_client = engine_client;
        hook_state.installed = true;
        return true;
    }

    void Remove() noexcept
    {
        std::scoped_lock lock(hook_state.mutex);
        if (!hook_state.installed) return;
        WriteCode(hook_state.target, handler_prologue.data(), handler_prologue.size());
        // The trampoline stays: a packet being handled right now may still be running through it.
        hook_state.installed = false;
        hook_state.error = "voice messages are not installed";
    }

    bool IsInstalled() noexcept
    {
        std::scoped_lock lock(hook_state.mutex);
        return hook_state.installed;
    }

    void RegisterLuaApi(void* lua_state) noexcept
    {
        if (find_table(lua_state, lua_registry_index, "_LOADED._G.skeetsdk", 4) != nullptr) return;
        // Runs on every load and the GC is off, so do not allocate the closures again every time.
        get_field(lua_state, -1, "_voice_send");
        const bool registered = get_type(lua_state, -1) == lua_type_function;
        set_top(lua_state, -2);
        if (!registered)
        {
            push_cclosure(lua_state, &NativePoll, 0);
            set_field(lua_state, -2, "_voice_poll");
            push_cclosure(lua_state, &NativeSend, 0);
            set_field(lua_state, -2, "_voice_send");
        }
        set_top(lua_state, -2);
        InstallClientWrapper(lua_state);
    }
#else
    bool Install() noexcept { return false; }
    void Remove() noexcept {}
    bool IsInstalled() noexcept { return false; }
    void RegisterLuaApi(void*) noexcept {}
#endif
}
