#include "cheat_revealer.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace skeetsdk::cheat_revealer
{
#if defined(_M_IX86)
    namespace
    {
        // Lua C API in the mapped image (same addresses the Lua guard uses).
        constexpr std::uintptr_t lua_push_cclosure_address = 0x43432F73u;
        constexpr std::uintptr_t lua_set_field_address = 0x43435B37u;
        constexpr std::uintptr_t lua_push_string_address = 0x434370E4u;
        constexpr std::uintptr_t lua_push_lstring_address = 0x43426891u;
        constexpr std::uintptr_t lua_to_lstring_address = 0x43435597u;
        constexpr std::uintptr_t lua_set_top_address = 0x43426BDFu;
        constexpr std::uintptr_t lua_find_table_address = 0x43426632u;
        constexpr std::uintptr_t lua_type_address = 0x4343654Au;
        constexpr std::uintptr_t lua_get_field_address = 0x4342EBA2u;
        constexpr std::uintptr_t lua_load_buffer_address = 0x43426D2Fu;
        constexpr std::uintptr_t lua_pcall_address = 0x4342D9B9u;
        constexpr int lua_registry_index = -10000;
        constexpr int lua_type_boolean = 1;
        constexpr int lua_type_string = 4;
        constexpr int lua_type_function = 6;

        using LuaCFunction = int(__cdecl*)(void*);
        using PushCClosureFn = void(__cdecl*)(void*, LuaCFunction, int);
        using SetFieldFn = void(__cdecl*)(void*, int, const char*);
        using PushStringFn = void(__cdecl*)(void*, const char*);
        using PushLStringFn = void(__cdecl*)(void*, const char*, std::size_t);
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
        const auto to_lstring = reinterpret_cast<ToLStringFn>(lua_to_lstring_address);
        const auto set_top = reinterpret_cast<SetTopFn>(lua_set_top_address);
        const auto find_table = reinterpret_cast<FindTableFn>(lua_find_table_address);
        const auto get_type = reinterpret_cast<TypeFn>(lua_type_address);
        const auto get_field = reinterpret_cast<GetFieldFn>(lua_get_field_address);
        const auto load_buffer = reinterpret_cast<LoadBufferFn>(lua_load_buffer_address);
        const auto call_protected = reinterpret_cast<PCallFn>(lua_pcall_address);

        std::string_view ReadString(void* lua_state, int index) noexcept
        {
            if (get_type(lua_state, index) != lua_type_string) return {};
            std::size_t size = 0;
            const char* text = to_lstring(lua_state, index, &size);
            return text != nullptr ? std::string_view(text, size) : std::string_view{};
        }

        // Lower-case letters and digits, as the icon file names are.
        bool AppendName(std::wstring& out, std::string_view name)
        {
            if (name.empty() || name.size() > 16) return false;
            for (const char c : name)
            {
                if (c >= 'a' && c <= 'z') out += static_cast<wchar_t>(c - 'a' + 'A');
                else if (c >= '0' && c <= '9') out += static_cast<wchar_t>(c);
                else return false;
            }
            return true;
        }

        // skeetsdk._revealer_icon(set, cheat) -> the embedded PNG (RV_<SET>_<CHEAT> in revealer_icons.rc), or nil
        int __cdecl NativeIcon(void* lua_state) noexcept
        {
            try
            {
                std::wstring resource = L"RV_";
                if (!AppendName(resource, ReadString(lua_state, 1))) return 0;
                resource += L'_';
                if (!AppendName(resource, ReadString(lua_state, 2))) return 0;
                const auto module = reinterpret_cast<HMODULE>(&__ImageBase);
                const HRSRC found = FindResourceW(module, resource.c_str(), MAKEINTRESOURCEW(10)); // RT_RCDATA
                const HGLOBAL loaded = found != nullptr ? LoadResource(module, found) : nullptr;
                const void* bytes = loaded != nullptr ? LockResource(loaded) : nullptr;
                if (bytes == nullptr) return 0;
                push_lstring(lua_state, static_cast<const char*>(bytes), SizeofResource(module, found));
                return 1;
            }
            catch (...)
            {
                return 0;
            }
        }

        // Cheat_revealer.lua without its workshop libraries: gamesense/uilib becomes ui.*, gamesense/color a small
        // { r, g, b, a } table, gamesense/http and the icon download the embedded icons, and its panorama code
        // entity.set_icon. The detectors (detector_table) and the voice handler are the script's, unchanged.
        // Two literals because MSVC caps one at 16 KB.
        constexpr char client_wrapper_source[] = R"LUA(
local sdk = skeetsdk
if sdk._revealer_installed ~= nil then return end
if type(client) ~= "table" or type(ui) ~= "table" or type(entity) ~= "table" or type(globals) ~= "table" then return end
if type(sdk.set_icon) ~= "function" or type(sdk._icon_write) ~= "function" or type(client.set_event_callback) ~= "function" then return end
local ffi_ok, ffi = pcall(require, "ffi")
if not ffi_ok or type(ffi) ~= "table" then return end
local bit = bit
if type(bit) ~= "table" then
    local bit_ok
    bit_ok, bit = pcall(require, "bit")
    if not bit_ok or type(bit) ~= "table" then return end
end
sdk._revealer_installed = false

local embedded_icon, write_icon, set_icon = sdk._revealer_icon, sdk._icon_write, sdk.set_icon

local color_methods = { unpack = function(self) return self.r, self.g, self.b, self.a end }
local color_mt = { __index = color_methods }
local function hex(text)
    return setmetatable({
        r = tonumber(text:sub(1, 2), 16), g = tonumber(text:sub(3, 4), 16),
        b = tonumber(text:sub(5, 6), 16), a = tonumber(text:sub(7, 8), 16)
    }, color_mt)
end

local function contains(list, value)
    for i = 1, #list do
        if list[i] == value then return true end
    end
    return false
end

local voice_data_t = ffi.typeof([[
	struct {
		char		 pad_0000[8];
		int32_t	client;
		int32_t	audible_mask;
		uint32_t xuid_low;
		uint32_t xuid_high;
		void*		voice_data;
		bool		 proximity;
		bool		 caster;
		char		 pad_001E[2];
		int32_t	format;
		int32_t	sequence_bytes;
		uint32_t section_number;
		uint32_t uncompressed_sample_offset;
		char		 pad_0030[4];
		uint32_t has_bits;
	} *
]])

local tab = "VISUALS"
local container = "Player ESP"

local names = {
    gs = { long = "gamesense", color = hex("95B80CFF") },
    nl = { long = "neverlose", color = hex("037696FF") },
    nw = { long = "nixware", color = hex("FFFFFFFF") },
    pd = { long = "pandora", color = hex("D4A9FFFF") },
    pr = { long = "primordial", color = hex("E2B6C7FF") },
    ot = { long = "onetap", color = hex("f7a414FF") },
    ft = { long = "fatality", color = hex("f00657FF") },
    pl = { long = "plaguecheat", color = hex("6BFF87FF") },
    ev = { long = "ev0lve", color = hex("42B7FFFF") },
    r7 = { long = "rifk7", color = hex("FF00FFFF") },
    af = { long = "airflow", color = hex("8E76C0FF") },
    wh = { long = "unknown", color = hex("9F9F9FFF") }
}
local colored_names = {
    gs = { short = "\aEAEAEAFFG\a95B80CFFS", long = "\aEAEAEAFFgame\a95B80CFFsense" },
    nl = { short = "\a037696FFNL", long = "\aEAEAEAFFnever\a037696FFlose" },
    nw = { short = "\aFFFFFFFFNW", long = "\aFFFFFFFFnixware" },
    pd = { short = "\aD4A9FFFFPD", long = "\aD4A9FFFFpandora" },
    pr = { short = "\aE2B6C7FFPR", long = "\aE2B6C7FFprimordial" },
    ot = { short = "\aEAEAEAFFO\af7a414FFT", long = "\aEAEAEAFFone\af7a414FFtap" },
    ft = { short = "\af00657FFFT", long = "\aF00657FFfatality" },
    pl = { short = "\a6BFF87FFPLG", long = "\a6BFF87FFplaguecheat" },
    ev = { short = "\a42B7FFFFEV0", long = "\a42B7FFFFev0\aFFFFFFFFlve" },
    r7 = { short = "\a00F600FFR\aFF00FFFF7", long = "\a00F600FFrifk\aFF00FFFF7" },
    af = { short = "\a8E76C0FFAF", long = "\a8E76C0FFairflow" },
    wh = { short = false, long = "\a515364FFunknown" }
}

local users = {}

local main = ui.new_checkbox(tab, container, "Cheat revealer")
local display_method = ui.new_multiselect(tab, container, "\nCheat revealer display options", "Scoreboard icon", "Flag")
local icon_items = { "Multicolored", "Unicolored", "Nado & Ryha", "Alternative NL icon" }
local icon_type = ui.new_multiselect(tab, container, "\nCheat revealer icon set", icon_items)
local list_label = ui.new_label("PLAYERS", "Adjustments", "Cheat: " .. colored_names.wh.long)
local player_list_ok, player_list = pcall(ui.reference, "PLAYERS", "Players", "Player list")
if not player_list_ok then player_list = nil end

local scoreboard_icon_enabled = false
local esp_flag_enabled = false

-- icon set, and the scoreboard icons written so far: "<set>/<cheat>" -> panorama image path
local is_multicolored, is_unicolored, is_nadoryha = false, false, false
local nl_path = "nl1"
local icon_set_name = "multicolored"
local icon_paths = {}

local function icon_path(cheat)
    local key = icon_set_name .. "/" .. cheat
    local path = icon_paths[key]
    if path == nil then
        local bytes = embedded_icon(icon_set_name, cheat)
        path = bytes ~= nil and write_icon(bytes, "png") or false
        icon_paths[key] = path
    end
    return path or nil
end

local function hide_icons()
    for target, user in pairs(users) do
        if user.icon_shown then
            set_icon(target, nil)
            user.icon_shown = nil
        end
        user.icon_set = false
    end
end

local function info_update_callback()
    local enabled = ui.get(main)
    ui.set_visible(icon_type, enabled)
    ui.set_visible(display_method, enabled)
    local method = ui.get(display_method)
    local was_scoreboard_icon_enabled = scoreboard_icon_enabled
    scoreboard_icon_enabled = enabled and contains(method, "Scoreboard icon")
    esp_flag_enabled = enabled and contains(method, "Flag")

    if was_scoreboard_icon_enabled and not scoreboard_icon_enabled then
        hide_icons()
    end
end

-- One icon set at a time; "Alternative NL icon" goes with the first two only (the script's rules).
local updating_icon_type = false
local function icon_type_callback()
    if updating_icon_type then return end
    local selected = {}
    for _, name in ipairs(ui.get(icon_type)) do selected[name] = true end

    if selected["Unicolored"] and (is_multicolored or is_nadoryha) then
        selected["Multicolored"], selected["Nado & Ryha"] = nil, nil
    elseif selected["Multicolored"] and (is_unicolored or is_nadoryha) then
        selected["Unicolored"], selected["Nado & Ryha"] = nil, nil
    elseif selected["Nado & Ryha"] and (is_multicolored or is_unicolored) then
        selected["Unicolored"], selected["Multicolored"], selected["Alternative NL icon"] = nil, nil, nil
    elseif selected["Alternative NL icon"] and is_nadoryha then
        selected["Alternative NL icon"] = nil
    else
        selected[is_multicolored and "Multicolored" or is_unicolored and "Unicolored" or is_nadoryha and "Nado & Ryha" or "Multicolored"] = true
    end

    local list = {}
    for _, name in ipairs(icon_items) do
        if selected[name] then list[#list + 1] = name end
    end
    updating_icon_type = true
    ui.set(icon_type, list)
    updating_icon_type = false

    is_nadoryha = selected["Nado & Ryha"] == true
    is_unicolored = selected["Unicolored"] == true
    is_multicolored = selected["Multicolored"] == true
    nl_path = not is_nadoryha and (selected["Alternative NL icon"] and "nl2" or "nl1") or "nl"
    icon_set_name = is_multicolored and "multicolored" or is_unicolored and "unicolored" or is_nadoryha and "nadoryha" or "multicolored"

    for _, user in pairs(users) do
        user.icon_set = false
    end
    info_update_callback()
end

ui.set_callback(main, info_update_callback)
ui.set_callback(display_method, info_update_callback)
ui.set_callback(icon_type, icon_type_callback)
-- Only when nothing is chosen: the chunk runs again in every new Lua state (each script load or unload).
if #ui.get(icon_type) == 0 then
    ui.set(icon_type, { "Multicolored" })
end
icon_type_callback()
info_update_callback()

local function get_players()
    local players = {}
    local player_resource = entity.get_player_resource()

    for i = 1, globals.maxplayers() do
        repeat
            if entity.get_prop(player_resource, "m_bConnected", i) == 0 then
                if users[i] then
                    if users[i].icon_shown then set_icon(i, nil) end
                    users[i] = nil
                end

                break
            else
                local flags = entity.get_prop(i, "m_fFlags")
                if not flags then
                    break
                end

                if bit.band(flags, 512) == 512 then
                    break
                end
            end

            players[#players + 1] = i
        until true
    end

    return players
end

local function find_duplicate_element(array, divisor)
    local visited_elements = {}

    for current_index = 1, #array do
        local current_element = array[current_index]

        if not visited_elements[current_element] then
            visited_elements[current_element] = true

            for next_index = current_index + 4, #array do
                if current_index % divisor == 0 then
                    if array[next_index] == current_element then
                        return true
                    end
                elseif array[next_index] == current_element then
                    return false
                end
            end
        end
    end

    return false
end
)LUA" R"LUA(
local detection_storage_table = {
    nl = {
        sig_count = {},
        found = {}
    },
    nw = {},
    pd = {},
    ot = {},
    ft = {},
    pl = {},
    ev = {},
    r7 = {},
    af = {},
    gs = {}
}
local detector_table = {
    nl = function(packet, target)
        if packet.xuid_high == 0 then
            return
        end

        local sig = ("%.02X"):format(ffi.cast("uint16_t*", ffi.cast("uintptr_t", packet) + 22)[0])

        if sig == detection_storage_table.current_signature then
            detection_storage_table.nl.sig_count[target] = (detection_storage_table.nl.sig_count[target] or 0) + 1

            if detection_storage_table.nl.sig_count[target] > 24 then
                detection_storage_table.nl.found[target] = 1

                return true
            else
                detection_storage_table.nl.sig_count[target] = nil
            end
        end

        if #detection_storage_table.nl.found > 3 then
            return false
        end

        if not detection_storage_table.nl[target] then
            detection_storage_table.nl[target] = {}
        end

        detection_storage_table.nl[target][#detection_storage_table.nl[target] + 1] = packet.xuid_high

        if #detection_storage_table.nl[target] > 24 then
            if find_duplicate_element(detection_storage_table.nl[target], 4) and packet.xuid_high ~= 0 then
                detection_storage_table.current_signature = sig
                detection_storage_table.nl[target] = {}

                return true
            end

            table.remove(detection_storage_table.nl[target], 1)
        end

        return false
    end,
    nw = function(packet, target)
        if not detection_storage_table.nw[target] then
            detection_storage_table.nw[target] = 0
        end

        if detection_storage_table.nw[target] > 34 then
            detection_storage_table.nw[target] = nil

            return true
        elseif packet.xuid_high == 0 then
            detection_storage_table.nw[target] = detection_storage_table.nw[target] + 1
        else
            detection_storage_table.nw[target] = 0
        end

        return false
    end,
    pd = function(packet, target)
        if not detection_storage_table.pd[target] then
            detection_storage_table.pd[target] = 0
        end

        local sig = ("%.02X"):format(ffi.cast("uint16_t*", ffi.cast("uintptr_t", packet) + 16)[0])

        if detection_storage_table.pd[target] > 24 then
            return true
        elseif sig == "695B" or sig == "1B39" then
            detection_storage_table.pd[target] = detection_storage_table.pd[target] + 1
        else
            detection_storage_table.pd[target] = 0
        end

        return false
    end,
    ot = function(packet, target)
        if not detection_storage_table.ot[target] then
            detection_storage_table.ot[target] = {}
        end

        detection_storage_table.ot[target][#detection_storage_table.ot[target] + 1] = {
            sequence_bytes = packet.sequence_bytes,
            xuid_low = packet.xuid_low,
            section_number = packet.section_number,
            umcompressed_sample_offset = packet.uncompressed_sample_offset
        }

        if #detection_storage_table.ot[target] > 16 then
            local oldest_packet = detection_storage_table.ot[target][1]

            for i = 2, #detection_storage_table.ot[target] do
                local loop_packet = detection_storage_table.ot[target][i]
                if loop_packet.xuid_low ~= oldest_packet.xuid_low or loop_packet.section_number ~= oldest_packet.section_number or loop_packet.uncompressed_sample_offset ~= oldest_packet.uncompressed_sample_offset then
                    table.remove(detection_storage_table.ot[target], 1)

                    return false
                end
            end

            table.remove(detection_storage_table.ot[target], 1)

            return true
        end

        return false
    end,
    ft = function(packet, target)
        if not detection_storage_table.ft[target] then
            detection_storage_table.ft[target] = 0
        end

        local sig = ("%.02X"):format(ffi.cast("uint16_t*", ffi.cast("uintptr_t", packet) + 16)[0])

        if detection_storage_table.ft[target] > 36 then
            return true
        elseif sig == "7FFA" or sig == "7FFB" then
            detection_storage_table.ft[target] = detection_storage_table.ft[target] + 1
        end

        return false
    end,
    pl = function(packet, target)
        if not detection_storage_table.pl[target] then
            detection_storage_table.pl[target] = 0
        end

        if detection_storage_table.pl[target] > 24 then
            return true
        elseif ("%.02X"):format(ffi.cast("uint16_t*", ffi.cast("uintptr_t", packet) + 44)[0]) == "7275" then
            detection_storage_table.pl[target] = detection_storage_table.pl[target] + 1
        else
            detection_storage_table.pl[target] = 0
        end

        return false
    end,
    ev = function(packet, target)
        if not detection_storage_table.ev[target] then
            detection_storage_table.ev[target] = {}
        end

        detection_storage_table.ev[target][#detection_storage_table.ev[target] + 1] = packet.xuid_high

        if #detection_storage_table.ev[target] > 44 then
            for i = 1, #detection_storage_table.ev[target] - 4 do
                local loop_info = detection_storage_table.ev[target][i]
                if detection_storage_table.ev[target][i + 1] + detection_storage_table.ev[target][i + 2] == detection_storage_table.ev[target][i] * 2 and detection_storage_table.ev[target][i + 4] == loop_info + 1 then
                    detection_storage_table.ev[target] = {}

                    return true
                end
            end

            table.remove(detection_storage_table.ev[target], 1)
        end

        return false
    end,
    r7 = function(packet, target)
        if not detection_storage_table.r7[target] then
            detection_storage_table.r7[target] = 0
        end

        local sig = ("%.02X"):format(ffi.cast("uint16_t*", ffi.cast("uintptr_t", packet) + 16)[0])

        if detection_storage_table.r7[target] > 24 then
            return true
        elseif sig == "234" or sig == "134" then
            detection_storage_table.r7[target] = detection_storage_table.r7[target] + 1
        else
            detection_storage_table.r7[target] = 0
        end

        return false
    end,
    af = function(packet, target)
        if not detection_storage_table.af[target] then
            detection_storage_table.af[target] = 0
        end

        if detection_storage_table.af[target] > 24 then
            return true
        elseif ("%.02X"):format(ffi.cast("uint16_t*", ffi.cast("uintptr_t", packet) + 16)[0]) == "AFF1" then
            detection_storage_table.af[target] = detection_storage_table.af[target] + 1
        else
            detection_storage_table.af[target] = 0
        end

        return false
    end,
    gs = function(packet, target)
        local sig = ("%.02X"):format(ffi.cast("uint16_t*", ffi.cast("uintptr_t", packet) + 22)[0])
        local sequence_bytes = string.sub(packet.sequence_bytes, 1, 4)

        if not detection_storage_table.gs[target] then
            detection_storage_table.gs[target] = {
                repeated = 0,
                packet = sig,
                bytes = sequence_bytes
            }
        end

        if sequence_bytes ~= detection_storage_table.gs[target].bytes and sig ~= detection_storage_table.gs[target].packet then
            detection_storage_table.gs[target].packet = sig
            detection_storage_table.gs[target].bytes = sequence_bytes
            detection_storage_table.gs[target].repeated = detection_storage_table.gs[target].repeated + 1
        else
            detection_storage_table.gs[target].repeated = 0
        end

        if detection_storage_table.gs[target].repeated >= 36 then
            detection_storage_table.gs[target] = {
                repeated = 0,
                packet = sig,
                bytes = sequence_bytes
            }

            return true
        end

        return false
    end
}

if type(client.register_esp_flag) == "function" then
    client.register_esp_flag("", 220, 220, 220, function(target)
        if not ui.get(main) then
            return false
        end

        if not users[target] or not users[target].cheat then
            return false
        end

        local cheat = users[target].cheat or "wh"

        if not esp_flag_enabled or not cheat or cheat == "wh" then
            return false
        end

        return true, (entity.is_dormant(target) and cheat or colored_names[cheat].short or ""):upper()
    end)
end

client.set_event_callback("paint", function()
    if not ui.get(main) or not scoreboard_icon_enabled then
        return
    end

    for _, target in pairs(get_players()) do
        local user = users[target]
        if user then
            if not user.icon_set then
                local path = icon_path(user.cheat and (user.cheat == "nl" and nl_path or user.cheat) or target == entity.get_local_player() and "gs" or "wh")
                if path then
                    set_icon(target, path)
                    user.icon_shown = true
                end

                user.icon_set = true
            end
        else
            users[target] = {}
        end
    end
end)

-- The player list's "Cheat: ..." label follows the selected player.
local last_label_text
client.set_event_callback("paint_ui", function()
    local selected = player_list and ui.get(player_list)
    local user = selected and users[selected]
    local text = "Cheat: " .. colored_names[user and user.cheat or "wh"].long
    if text ~= last_label_text then
        last_label_text = text
        ui.set(list_label, text)
    end
end)

client.set_event_callback("voice", function(event)
    if not ui.get(main) then
        return
    end

    local packet = ffi.cast(voice_data_t, event.data)
    local target = (ffi.cast("char*", packet) + 8)[0] + 1
    if not users[target] then
        users[target] = {}
    end

    local user = users[target]

    for cheat_identifier, cheat_detection_function in pairs(detector_table) do
        repeat
            local cheat = user.cheat
            if user.cheat ~= cheat_identifier and (cheat_identifier ~= "nl" or user.cheat ~= "ev" and user.cheat ~= "gs" and user.cheat ~= "pl" and user.cheat ~= "pd" and user.cheat ~= "r7" and user.cheat ~= "af" and user.cheat ~= "ft") and (cheat_identifier ~= "nw" or user.cheat ~= "nl") and (cheat_identifier ~= "ev" or user.cheat ~= "pd" and user.cheat ~= "nl" and user.cheat ~= "ft") and (cheat_identifier ~= "gs" or user.cheat ~= "ev" and user.cheat ~= "ot" and user.cheat ~= "pl" and user.cheat ~= "pd" and user.cheat ~= "r7" and user.cheat ~= "ft") and (cheat_identifier ~= "ot" or user.cheat ~= "nw" and user.cheat ~= "ft" and user.cheat ~= "pd" and user.cheat ~= "pl") then
                if cheat_identifier == "ft" and (user.cheat == "nw" or user.cheat == "pd") then
                    break
                end

                if cheat_detection_function(packet, target) then
                    user.cheat = cheat_identifier
                    user.icon_set = false

                    if ((user.cheat or "wh") == "wh" or cheat ~= cheat_identifier) and type(client.fire_event) == "function" then
                        client.fire_event("cheat_detected", {
                            player = target,
                            cheat_id = cheat_identifier,
                            cheat_long = names[cheat_identifier].long,
                            cheat_color = names[cheat_identifier].color
                        })
                    end
                end
            end
        until true
    end
end)

client.set_event_callback("player_connect_full", function(event)
    if not ui.get(main) then
        return
    end

    local target = client.userid_to_entindex(event.userid)
    if target == entity.get_local_player() then
        users = {}
    elseif target ~= nil then
        users[target] = {}
        for _, detection in pairs(detection_storage_table) do
            if type(detection) == "table" then detection[target] = nil end
        end
    end
end)

client.set_event_callback("game_start", function()
    if not ui.get(main) then
        return
    end

    for _, user in pairs(users) do
        user.icon_set = false
    end
end)

if type(package) == "table" and type(package.preload) == "table" then
    package.preload["gamesense/cheat_revealer"] = function()
        return {
            get_cheat = function(target)
                local cheat = users[target] and users[target].cheat or "wh"

                return {
                    cheat_id = cheat,
                    cheat_long = names[cheat].long or "unknown",
                    cheat_color = names[cheat].color or hex("9F9F9FFF")
                }
            end,
            has_data = function(target)
                return users[target] ~= nil
            end,
            clear_data = function(target)
                if users[target] == nil then
                    return false
                end

                users[target] = nil

                for _, detection in pairs(detection_storage_table) do
                    if type(detection) == "table" then detection[target] = nil end
                end

                return true
            end
        }
    end
end

sdk._revealer_installed = true
)LUA";

        bool installing_wrapper = false;

        // Runs on every load until it has run once in this Lua state (client / ui / entity may not exist yet on the
        // first loads). luaL_loadbuffer goes through the Lua guard's lua_loadx hook, which calls RegisterLuaApi
        // again, hence the flag. The chunk marks itself as soon as it starts making controls, so a chunk that
        // failed halfway is not run again into duplicate controls.
        void InstallRevealer(void* lua_state) noexcept
        {
            if (installing_wrapper) return;
            if (find_table(lua_state, lua_registry_index, "_LOADED._G.skeetsdk", 4) != nullptr) return;
            get_field(lua_state, -1, "_revealer_installed");
            const bool done = get_type(lua_state, -1) == lua_type_boolean;
            set_top(lua_state, -3);
            if (done) return;
            installing_wrapper = true;
            if (load_buffer(lua_state, client_wrapper_source, sizeof(client_wrapper_source) - 1, "=cheat_revealer") == 0)
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

    void RegisterLuaApi(void* lua_state) noexcept
    {
        if (find_table(lua_state, lua_registry_index, "_LOADED._G.skeetsdk", 4) != nullptr) return;
        // Runs on every load and the GC is off, so do not allocate the closure again every time.
        get_field(lua_state, -1, "_revealer_icon");
        const bool registered = get_type(lua_state, -1) == lua_type_function;
        set_top(lua_state, -2);
        if (!registered)
        {
            push_cclosure(lua_state, &NativeIcon, 0);
            set_field(lua_state, -2, "_revealer_icon");
        }
        set_top(lua_state, -2);
        InstallRevealer(lua_state);
    }
#else
    void RegisterLuaApi(void*) noexcept {}
#endif
}
