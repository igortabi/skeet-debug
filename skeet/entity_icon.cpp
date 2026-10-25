#include "entity_icon.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <string_view>

namespace skeetsdk::entity_icon
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
        constexpr int lua_type_number = 3;
        constexpr int lua_type_string = 4;
        constexpr int lua_type_function = 6;

        constexpr std::size_t max_bytes = 16u * 1024u * 1024u;
        constexpr int max_side = 4096;

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

        // lua_State: base at +0x10, top at +0x14. index is 1-based like the Lua API.
        const TValue* Argument(void* lua_state, int index) noexcept
        {
            const auto state = static_cast<const std::uint8_t*>(lua_state);
            const auto stack_base = *reinterpret_cast<TValue* const*>(state + 0x10);
            const auto stack_top = *reinterpret_cast<TValue* const*>(state + 0x14);
            if (index < 1 || stack_base == nullptr || stack_top - stack_base < index) return nullptr;
            return stack_base + (index - 1);
        }

        bool ReadWhole(void* lua_state, int index, int low, int high, int& value) noexcept
        {
            const TValue* argument = Argument(lua_state, index);
            if (argument == nullptr || get_type(lua_state, index) != lua_type_number) return false;
            double number = 0.0;
            std::memcpy(&number, argument, sizeof(number));
            if (!std::isfinite(number) || number != std::floor(number) || number < low || number > high) return false;
            value = static_cast<int>(number);
            return true;
        }

        std::string_view ReadString(void* lua_state, int index) noexcept
        {
            if (get_type(lua_state, index) != lua_type_string) return {};
            std::size_t size = 0;
            const char* text = to_lstring(lua_state, index, &size);
            return text != nullptr ? std::string_view(text, size) : std::string_view{};
        }

        int Fail(void* lua_state, const char* message) noexcept
        {
            push_string(lua_state, nullptr);
            push_string(lua_state, message);
            return 2;
        }

        // --- PNG -------------------------------------------------------------------------------------

        std::uint32_t Crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc = 0) noexcept
        {
            static const auto table = [] {
                std::array<std::uint32_t, 256> values{};
                for (std::uint32_t i = 0; i < 256; ++i)
                {
                    std::uint32_t c = i;
                    for (int k = 0; k < 8; ++k) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    values[i] = c;
                }
                return values;
            }();
            crc = ~crc;
            for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
            return ~crc;
        }

        void PutBig(std::string& out, std::uint32_t value)
        {
            out += static_cast<char>(value >> 24);
            out += static_cast<char>(value >> 16);
            out += static_cast<char>(value >> 8);
            out += static_cast<char>(value);
        }

        void Chunk(std::string& out, const char* type, const std::string& data)
        {
            PutBig(out, static_cast<std::uint32_t>(data.size()));
            const std::size_t start = out.size();
            out.append(type, 4);
            out += data;
            PutBig(out, Crc32(reinterpret_cast<const std::uint8_t*>(out.data() + start), out.size() - start));
        }

        // RGBA (what renderer.load_rgba takes) as a PNG: no filter, zlib with stored (uncompressed) blocks.
        std::string EncodePng(std::string_view rgba, int width, int height)
        {
            std::string raw;
            raw.reserve(static_cast<std::size_t>(height) * (static_cast<std::size_t>(width) * 4 + 1));
            const std::size_t row = static_cast<std::size_t>(width) * 4;
            for (int y = 0; y < height; ++y)
            {
                raw += '\0';
                raw.append(rgba.substr(static_cast<std::size_t>(y) * row, row));
            }

            std::string zlib = "\x78\x01";
            std::uint32_t a = 1, b = 0;
            for (const char c : raw)
            {
                a = (a + static_cast<std::uint8_t>(c)) % 65521;
                b = (b + a) % 65521;
            }
            for (std::size_t offset = 0; offset < raw.size() || offset == 0;)
            {
                const std::size_t size = std::min<std::size_t>(raw.size() - offset, 65535);
                const bool last = offset + size >= raw.size();
                zlib += static_cast<char>(last ? 1 : 0);
                zlib += static_cast<char>(size & 0xFF);
                zlib += static_cast<char>(size >> 8);
                zlib += static_cast<char>(~size & 0xFF);
                zlib += static_cast<char>((~size >> 8) & 0xFF);
                zlib.append(raw, offset, size);
                offset += size;
                if (last) break;
            }
            PutBig(zlib, (b << 16) | a);

            std::string header;
            PutBig(header, static_cast<std::uint32_t>(width));
            PutBig(header, static_cast<std::uint32_t>(height));
            header.append("\x08\x06\x00\x00\x00", 5);  // 8 bits, RGBA, deflate, no filter, no interlace

            std::string png = "\x89PNG\r\n\x1A\n";
            Chunk(png, "IHDR", header);
            Chunk(png, "IDAT", zlib);
            Chunk(png, "IEND", {});
            return png;
        }

        // --- files -----------------------------------------------------------------------------------

        // csgo\materials\panorama\images\icons\skeetsdk next to csgo.exe, made if missing; empty on failure.
        std::wstring IconDirectory()
        {
            wchar_t executable[MAX_PATH]{};
            const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
            if (length == 0 || length >= MAX_PATH) return {};
            std::wstring directory(executable, length);
            directory.resize(directory.find_last_of(L'\\'));
            for (const wchar_t* part : { L"\\csgo", L"\\materials", L"\\panorama", L"\\images", L"\\icons", L"\\skeetsdk" })
            {
                directory += part;
                if (!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return {};
            }
            return directory;
        }

        std::uint64_t Fnv1a(std::string_view bytes) noexcept
        {
            std::uint64_t hash = 0xCBF29CE484222325ull;
            for (const char c : bytes) hash = (hash ^ static_cast<std::uint8_t>(c)) * 0x100000001B3ull;
            return hash;
        }

        // skeetsdk._icon_write(bytes, extension) -> panorama image path, or nil and a message. The name is the hash
        // of the bytes, so panorama's image cache never shows an older picture under the same path.
        int __cdecl NativeWrite(void* lua_state) noexcept
        {
            const std::string_view bytes = ReadString(lua_state, 1);
            const std::string_view extension = ReadString(lua_state, 2);
            if (bytes.empty() || bytes.size() > max_bytes) return Fail(lua_state, "bad image data");
            if (extension != "png" && extension != "jpg" && extension != "svg") return Fail(lua_state, "bad image type");
            try
            {
                const std::wstring directory = IconDirectory();
                if (directory.empty()) return Fail(lua_state, "could not make the icon folder");
                const std::string name = std::format("{:016x}.{}", Fnv1a(bytes), extension);
                const std::wstring path = directory + L"\\" + std::wstring(name.begin(), name.end());
                WIN32_FILE_ATTRIBUTE_DATA existing{};
                const bool written = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &existing)
                    && existing.nFileSizeHigh == 0 && existing.nFileSizeLow == bytes.size();
                if (!written)
                {
                    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (file == INVALID_HANDLE_VALUE) return Fail(lua_state, "could not write the icon file");
                    DWORD done = 0;
                    const BOOL ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &done, nullptr);
                    CloseHandle(file);
                    if (!ok || done != bytes.size()) return Fail(lua_state, "could not write the icon file");
                }
                const std::string image = "file://{images}/icons/skeetsdk/" + name;
                push_lstring(lua_state, image.data(), image.size());
                return 1;
            }
            catch (...)
            {
                return Fail(lua_state, "out of memory");
            }
        }

        // skeetsdk._icon_png(rgba, width, height) -> PNG bytes, or nil and a message
        int __cdecl NativePng(void* lua_state) noexcept
        {
            const std::string_view rgba = ReadString(lua_state, 1);
            int width = 0, height = 0;
            if (!ReadWhole(lua_state, 2, 1, max_side, width) || !ReadWhole(lua_state, 3, 1, max_side, height))
                return Fail(lua_state, "bad size");
            if (rgba.size() != static_cast<std::size_t>(width) * height * 4) return Fail(lua_state, "the RGBA data does not match the size");
            try
            {
                const std::string png = EncodePng(rgba, width, height);
                push_lstring(lua_state, png.data(), png.size());
                return 1;
            }
            catch (...)
            {
                return Fail(lua_state, "out of memory");
            }
        }

        // renderer.load_* wrappers, set_icon and the scoreboard code. The panorama part is the icon render of
        // Cheat_revealer.lua (its rank-column slot layout and row lookup), plus remove and apply-now.
        constexpr char client_wrapper_source[] = R"LUA(
local sdk = skeetsdk
if type(sdk.set_icon) == "function" then return end
if type(entity) ~= "table" or type(renderer) ~= "table" or type(panorama) ~= "table" or type(panorama.loadstring) ~= "function" then return end
if type(client) ~= "table" or type(client.set_event_callback) ~= "function" then return end
local write_icon, png_from_rgba = sdk._icon_write, sdk._icon_png
local type, error, tostring, floor = type, error, tostring, math.floor

-- texture id -> what it was made from, then the panorama image path once it was written
local sources = {}

local function wrap(name, kind)
    local original = renderer[name]
    if type(original) ~= "function" then return end
    renderer[name] = function(contents, width, height, ...)
        local id = original(contents, width, height, ...)
        if id ~= nil and type(contents) == "string" then
            sources[id] = { kind = kind, contents = contents, width = width, height = height }
        end
        return id
    end
end
wrap("load_png", "png")
wrap("load_jpg", "jpg")
wrap("load_svg", "svg")
wrap("load_rgba", "rgba")

local function image_path(source)
    if source.path == nil then
        local bytes, kind, message = source.contents, source.kind
        if kind == "rgba" then
            bytes, message = png_from_rgba(bytes, source.width, source.height)
            if bytes == nil then return nil, message end
            kind = "png"
        end
        local path
        path, message = write_icon(bytes, kind)
        if path == nil then return nil, message end
        source.path, source.contents = path, nil
    end
    return source.path
end

local scoreboard

local function get_scoreboard()
    if scoreboard ~= nil then return scoreboard end
    scoreboard = panorama.loadstring([==[
let entity_panels = {}
let entity_data = {}
let event_callbacks = {}
let SLOT_LAYOUT = `
    <root>
        <Panel style="min-width: 3px; padding-top: 2px; padding-left: 0px;" scaling='stretch-to-fit-y-preserve-aspect'>
            <Image id="smaller" textureheight="15" style="horizontal-align: center; opacity: 0.01; transition: opacity 0.1s ease-in-out 0.0s, img-shadow 0.12s ease-in-out 0.0s; overflow: noclip; padding: 3px 5px; margin: -3px -5px;" />
            <Image id="small" textureheight="17" style="horizontal-align: center; opacity: 0.01; transition: opacity 0.1s ease-in-out 0.0s, img-shadow 0.12s ease-in-out 0.0s; overflow: noclip; padding: 3px 5px; margin: -3px -5px;" />
            <Image id="image" textureheight="21" style="opacity: 0.01; transition: opacity 0.1s ease-in-out 0.0s, img-shadow 0.12s ease-in-out 0.0s; padding: 3px 5px; margin: -3px -5px; margin-top: -5px;" />
        </Panel>
    </root>
`
let _DestroyEntityPanel = function (key) {
    let panel = entity_panels[key]
    if(panel != null && panel.IsValid()) {
        let parent = panel.GetParent()
        let rank = parent.GetChild(0)
        if(rank != null)
            rank.visible = true
        if(parent.FindChildTraverse("id-sb-skillgroup-image") != null) {
            parent.FindChildTraverse("id-sb-skillgroup-image").style.margin = "0px 0px 0px 0px"
        }
        panel.DeleteAsync(0.0)
    }
    delete entity_panels[key]
}
let _DestroyEntityPanels = function() {
    for(let key in entity_panels){
        _DestroyEntityPanel(key)
    }
}
let _FindScoreboard = function() {
    let context = $.GetContextPanel()
    let container = context.FindChildTraverse("ScoreboardContainer")
    let scoreboard = container != null ? container.FindChildTraverse("Scoreboard") : null
    if(scoreboard == null) {
        container = context.FindChildTraverse("id-eom-scoreboard-container")
        scoreboard = container != null ? container.FindChildTraverse("Scoreboard") : null
    }
    return scoreboard
}
let _GetOrCreateCustomPanel = function(xuid) {
    if(entity_panels[xuid] == null || !entity_panels[xuid].IsValid()){
        entity_panels[xuid] = null
        let scoreboard_context_panel = _FindScoreboard()
        if(scoreboard_context_panel == null){
            _DestroyEntityPanels()
            return
        }
        scoreboard_context_panel.FindChildrenWithClassTraverse("sb-row").forEach(function(el){
            let scoreboard_el
            if(el.m_xuid == xuid) {
                el.Children().forEach(function(child_frame){
                    let stat = child_frame.GetAttributeString("data-stat", "")
                    if(stat == "rank")
                        scoreboard_el = child_frame.GetChild(0)
                })
                if(scoreboard_el) {
                    let scoreboard_el_parent = scoreboard_el.GetParent()
                    let custom_icons = $.CreatePanel("Panel", scoreboard_el_parent, "skeetsdk-icon", {})
                    if(scoreboard_el_parent.FindChildTraverse("id-sb-skillgroup-image") != null) {
                        scoreboard_el_parent.FindChildTraverse("id-sb-skillgroup-image").style.margin = "0px 0px 0px 0px"
                    }
                    scoreboard_el_parent.MoveChildAfter(custom_icons, scoreboard_el_parent.GetChild(1))
                    let prev_panel = scoreboard_el_parent.GetChild(0)
                    prev_panel.visible = false
                    let panel_slot_parent = $.CreatePanel("Panel", custom_icons, `icon`)
                    panel_slot_parent.visible = false
                    panel_slot_parent.BLoadLayoutFromString(SLOT_LAYOUT, false, false)
                    entity_panels[xuid] = custom_icons
                }
            }
        })
    }
    return entity_panels[xuid]
}
let _ApplyPlayer = function(entindex) {
    let xuid = GameStateAPI.GetPlayerXuidStringFromEntIndex(entindex)
    let panel = _GetOrCreateCustomPanel(xuid)
    if(panel == null)
        return false
    let panel_slot_parent = panel.FindChild(`icon`)
    panel_slot_parent.visible = true
    let panel_slot = panel_slot_parent.FindChild("image")
    panel_slot.visible = true
    panel_slot.style.opacity = "1"
    panel_slot.SetImage(entity_data[entindex].image_path)
    return true
}
let _ApplyData = function() {
    for(let key in entity_data) {
        let entindex = parseInt(key)
        let xuid = GameStateAPI.GetPlayerXuidStringFromEntIndex(entindex)
        if(!entity_data[entindex].applied || entity_panels[xuid] == null || !entity_panels[xuid].IsValid()) {
            if(_ApplyPlayer(entindex)) {
                entity_data[entindex].applied = true
            }
        }
    }
}
let _UpdatePlayer = function(entindex, path_to_image) {
    if(entindex == null || entindex == 0)
        return
    entity_data[entindex] = {
        applied: false,
        image_path: path_to_image
    }
    _ApplyData()
}
let _RemovePlayer = function(entindex) {
    if(entity_data[entindex] == null)
        return
    delete entity_data[entindex]
    _DestroyEntityPanel(GameStateAPI.GetPlayerXuidStringFromEntIndex(entindex))
}
let _Create = function() {
    event_callbacks["OnOpenScoreboard"] = $.RegisterForUnhandledEvent("OnOpenScoreboard", _ApplyData)
    event_callbacks["Scoreboard_UpdateEverything"] = $.RegisterForUnhandledEvent("Scoreboard_UpdateEverything", _ApplyData)
    event_callbacks["Scoreboard_UpdateJob"] = $.RegisterForUnhandledEvent("Scoreboard_UpdateJob", _ApplyData)
}
let _Clear = function() {
    entity_data = {}
    _DestroyEntityPanels()
}
let _Destroy = function() {
    _Clear()
    for(let event in event_callbacks){
        $.UnregisterForUnhandledEvent(event, event_callbacks[event])
        delete event_callbacks[event]
    }
}
return {
    create: _Create,
    destroy: _Destroy,
    clear: _Clear,
    update: _UpdatePlayer,
    remove: _RemovePlayer
}
]==], "CSGOHud")()
    scoreboard.create()
    client.set_event_callback("shutdown", function() scoreboard.destroy() end)
    -- A new server: other players. Someone else in a slot: not the player the icon was for.
    client.set_event_callback("player_connect_full", function(e)
        local player = client.userid_to_entindex(e.userid)
        if player == entity.get_local_player() then
            scoreboard.clear()
        elseif player ~= nil and player ~= 0 then
            scoreboard.remove(player)
        end
    end)
    return scoreboard
end

local function set_icon(player, icon)
    if type(player) ~= "number" or player ~= floor(player) or player < 1 or player > 64 then
        return nil, "bad argument #1 to 'set_icon' (player entity index expected)"
    end
    if icon == nil then
        if scoreboard ~= nil then scoreboard.remove(player) end
        return true
    end
    local path, message = icon, nil
    if type(icon) == "number" then
        local source = sources[icon]
        if source == nil then
            return nil, "bad argument #2 to 'set_icon' (texture id from renderer.load_png / load_jpg / load_svg / load_rgba expected)"
        end
        path, message = image_path(source)
        if path == nil then return nil, "set_icon: " .. tostring(message) end
    elseif type(icon) ~= "string" then
        return nil, "bad argument #2 to 'set_icon' (texture id expected)"
    end
    get_scoreboard().update(player, path)
    return true
end

sdk.set_icon = set_icon
entity.set_icon = function(player, icon)
    local ok, message = set_icon(player, icon)
    if not ok then error(message, 2) end
    return true
end
)LUA";

        bool installing_wrapper = false;

        // Runs on every load until it has worked once in this Lua state (entity / renderer / panorama may not exist
        // yet on the first loads). luaL_loadbuffer goes through the Lua guard's lua_loadx hook, which calls
        // RegisterLuaApi again, hence the flag.
        void InstallClientWrapper(void* lua_state) noexcept
        {
            if (installing_wrapper) return;
            if (find_table(lua_state, lua_registry_index, "_LOADED._G.skeetsdk", 4) != nullptr) return;
            get_field(lua_state, -1, "set_icon");
            const bool done = get_type(lua_state, -1) == lua_type_function;
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

    void RegisterLuaApi(void* lua_state) noexcept
    {
        if (find_table(lua_state, lua_registry_index, "_LOADED._G.skeetsdk", 4) != nullptr) return;
        // Runs on every load and the GC is off, so do not allocate the closures again every time.
        get_field(lua_state, -1, "_icon_write");
        const bool registered = get_type(lua_state, -1) == lua_type_function;
        set_top(lua_state, -2);
        if (!registered)
        {
            push_cclosure(lua_state, &NativeWrite, 0);
            set_field(lua_state, -2, "_icon_write");
            push_cclosure(lua_state, &NativePng, 0);
            set_field(lua_state, -2, "_icon_png");
        }
        set_top(lua_state, -2);
        InstallClientWrapper(lua_state);
    }
#else
    void RegisterLuaApi(void*) noexcept {}
#endif
}
