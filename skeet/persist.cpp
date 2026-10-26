#include "persist.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace skeetsdk::persist
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
        constexpr int lua_type_none = -1;
        constexpr int lua_type_nil = 0;
        constexpr int lua_type_string = 4;
        constexpr int lua_type_table = 5;
        constexpr int lua_type_function = 6;
        constexpr std::int32_t itype_true = -3;

        constexpr std::size_t max_total_bytes = 64u * 1024u * 1024u;
        constexpr std::size_t max_name_bytes = 256;

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

        // namespace -> key -> serialized value (the format of the Lua side below). Outlives every Lua state.
        std::mutex store_mutex;
        std::map<std::string, std::map<std::string, std::string, std::less<>>, std::less<>> store;
        std::size_t total_bytes = 0;

        bool ReadString(void* lua_state, int index, std::string_view& value) noexcept
        {
            if (get_type(lua_state, index) != lua_type_string) return false;
            std::size_t size = 0;
            const char* text = to_lstring(lua_state, index, &size);
            if (text == nullptr) return false;
            value = std::string_view(text, size);
            return true;
        }

        bool ReadName(void* lua_state, int index, std::string_view& value) noexcept
        {
            return ReadString(lua_state, index, value) && !value.empty() && value.size() <= max_name_bytes;
        }

        // A C function is guaranteed LUA_MINSTACK free slots. lua_State: top at +0x14.
        int ReturnTrue(void* lua_state) noexcept
        {
            auto** top = reinterpret_cast<TValue**>(static_cast<std::uint8_t*>(lua_state) + 0x14);
            (*top)->value = 0;
            (*top)->type = itype_true;
            ++*top;
            return 1;
        }

        int Fail(void* lua_state, const char* message) noexcept
        {
            push_string(lua_state, nullptr);
            push_string(lua_state, message);
            return 2;
        }

        std::size_t EntryBytes(std::string_view key, std::string_view value) noexcept
        {
            return key.size() + value.size();
        }

        // skeetsdk._persist_get(namespace, key) -> serialized value, or nothing
        int __cdecl NativeGet(void* lua_state) noexcept
        {
            std::string_view name, key;
            if (!ReadName(lua_state, 1, name) || !ReadName(lua_state, 2, key)) return 0;
            std::scoped_lock lock(store_mutex);
            const auto space = store.find(name);
            if (space == store.end()) return 0;
            const auto entry = space->second.find(key);
            if (entry == space->second.end()) return 0;
            push_lstring(lua_state, entry->second.data(), entry->second.size());
            return 1;
        }

        // skeetsdk._persist_set(namespace, key, serialized value or nil) -> true, or nil and a message
        int __cdecl NativeSet(void* lua_state) noexcept
        {
            std::string_view name, key, value;
            if (!ReadName(lua_state, 1, name)) return Fail(lua_state, "bad namespace (non-empty string of at most 256 bytes expected)");
            if (!ReadName(lua_state, 2, key)) return Fail(lua_state, "bad key (non-empty string of at most 256 bytes expected)");
            const int type = get_type(lua_state, 3);
            const bool remove = type == lua_type_nil || type == lua_type_none;
            if (!remove && !ReadString(lua_state, 3, value)) return Fail(lua_state, "bad value");
            try
            {
                std::scoped_lock lock(store_mutex);
                auto space = store.find(name);
                if (remove)
                {
                    if (space == store.end()) return ReturnTrue(lua_state);
                    const auto entry = space->second.find(key);
                    if (entry != space->second.end())
                    {
                        total_bytes -= EntryBytes(entry->first, entry->second);
                        space->second.erase(entry);
                        if (space->second.empty()) store.erase(space);
                    }
                    return ReturnTrue(lua_state);
                }
                std::size_t old_bytes = 0;
                if (space != store.end())
                {
                    const auto entry = space->second.find(key);
                    if (entry != space->second.end()) old_bytes = EntryBytes(entry->first, entry->second);
                }
                const std::size_t new_bytes = EntryBytes(key, value);
                if (total_bytes - old_bytes + new_bytes > max_total_bytes)
                    return Fail(lua_state, "skeetsdk.persist is full (64 MB)");
                if (space == store.end()) space = store.emplace(std::string(name), std::map<std::string, std::string, std::less<>>{}).first;
                space->second.insert_or_assign(std::string(key), std::string(value));
                total_bytes = total_bytes - old_bytes + new_bytes;
                return ReturnTrue(lua_state);
            }
            catch (...)
            {
                return Fail(lua_state, "out of memory");
            }
        }

        // skeetsdk._persist_keys(namespace) -> the keys joined with "\0" (sorted), or ""
        int __cdecl NativeKeys(void* lua_state) noexcept
        {
            std::string_view name;
            if (!ReadName(lua_state, 1, name)) return Fail(lua_state, "bad namespace (non-empty string of at most 256 bytes expected)");
            try
            {
                std::string joined;
                {
                    std::scoped_lock lock(store_mutex);
                    const auto space = store.find(name);
                    if (space != store.end())
                        for (const auto& [key, value] : space->second)
                        {
                            if (!joined.empty()) joined += '\0';
                            joined += key;
                        }
                }
                push_lstring(lua_state, joined.data(), joined.size());
                return 1;
            }
            catch (...)
            {
                return Fail(lua_state, "out of memory");
            }
        }

        // skeetsdk._persist_clear(namespace) -> true
        int __cdecl NativeClear(void* lua_state) noexcept
        {
            std::string_view name;
            if (!ReadName(lua_state, 1, name)) return Fail(lua_state, "bad namespace (non-empty string of at most 256 bytes expected)");
            std::scoped_lock lock(store_mutex);
            const auto space = store.find(name);
            if (space != store.end())
            {
                for (const auto& [key, value] : space->second) total_bytes -= EntryBytes(key, value);
                store.erase(space);
            }
            return ReturnTrue(lua_state);
        }

        // Values are stored serialized: t / f (booleans), N / I / J (nan, inf, -inf), d<len>:<%.17g> (numbers),
        // s<len>:<bytes> (strings), T<count>:<key><value>... (tables). Only this code writes them.
        constexpr char wrapper_source[] = R"LUA(
local sdk = skeetsdk
if type(sdk.persist) == "table" then return end
local get, set, list, clear = sdk._persist_get, sdk._persist_set, sdk._persist_keys, sdk._persist_clear
local type, error, pcall, next, tostring, tonumber, setmetatable = type, error, pcall, next, tostring, tonumber, setmetatable
local format, sub, find, concat, sort = string.format, string.sub, string.find, table.concat, table.sort
local huge = math.huge
local max_depth = 64

local function encode(value, out, depth, seen)
    local kind = type(value)
    if kind == "boolean" then
        out[#out + 1] = value and "t" or "f"
    elseif kind == "number" then
        if value ~= value then
            out[#out + 1] = "N"
        elseif value == huge then
            out[#out + 1] = "I"
        elseif value == -huge then
            out[#out + 1] = "J"
        else
            local text = format("%.17g", value)
            out[#out + 1] = "d" .. #text .. ":" .. text
        end
    elseif kind == "string" then
        out[#out + 1] = "s" .. #value .. ":"
        out[#out + 1] = value
    elseif kind == "table" then
        if seen[value] then error("a table that contains itself cannot be stored", 0) end
        if depth >= max_depth then error("tables nested more than " .. max_depth .. " levels deep cannot be stored", 0) end
        seen[value] = true
        local parts, count = {}, 0
        for key, item in next, value do
            local key_kind = type(key)
            if key_kind ~= "string" and key_kind ~= "number" and key_kind ~= "boolean" then
                error("a table key of type " .. key_kind .. " cannot be stored", 0)
            end
            count = count + 1
            encode(key, parts, depth + 1, seen)
            encode(item, parts, depth + 1, seen)
        end
        seen[value] = nil
        out[#out + 1] = "T" .. count .. ":"
        out[#out + 1] = concat(parts)
    else
        error("a " .. kind .. " cannot be stored", 0)
    end
end

local function decode(data, position)
    local tag = sub(data, position, position)
    if tag == "t" then return true, position + 1 end
    if tag == "f" then return false, position + 1 end
    if tag == "N" then return 0 / 0, position + 1 end
    if tag == "I" then return huge, position + 1 end
    if tag == "J" then return -huge, position + 1 end
    local colon = find(data, ":", position + 1, true)
    local length = colon and tonumber(sub(data, position + 1, colon - 1))
    if length == nil then error("skeetsdk.persist: damaged value", 0) end
    if tag == "d" then return tonumber(sub(data, colon + 1, colon + length)), colon + length + 1 end
    if tag == "s" then return sub(data, colon + 1, colon + length), colon + length + 1 end
    if tag == "T" then
        local result, at = {}, colon + 1
        for _ = 1, length do
            local key, item
            key, at = decode(data, at)
            item, at = decode(data, at)
            result[key] = item
        end
        return result, at
    end
    error("skeetsdk.persist: damaged value", 0)
end

local function check_name(name, what, level)
    if type(name) ~= "string" or name == "" or #name > 256 then
        error("bad " .. what .. " (non-empty string of at most 256 bytes expected)", level + 1)
    end
end

local stores = {}

local function open(namespace)
    check_name(namespace, "argument #1 to 'persist'", 2)
    local store = stores[namespace]
    if store ~= nil then return store end
    store = setmetatable({}, {
        __index = function(_, key)
            check_name(key, "persist key", 2)
            local data = get(namespace, key)
            if data == nil then return nil end
            return (decode(data, 1))
        end,
        __newindex = function(_, key, value)
            check_name(key, "persist key", 2)
            local data
            if value ~= nil then
                local out = {}
                local ok, message = pcall(encode, value, out, 0, {})
                if not ok then error("persist: " .. tostring(message), 2) end
                data = concat(out)
            end
            local done, message = set(namespace, key, data)
            if not done then error("persist: " .. tostring(message), 2) end
        end,
        __tostring = function() return "persist(" .. namespace .. ")" end,
        __metatable = false
    })
    stores[namespace] = store
    return store
end

local function keys(namespace)
    check_name(namespace, "argument #1 to 'keys'", 2)
    local joined = list(namespace) or ""
    local result = {}
    local start = 1
    while start <= #joined do
        local stop = find(joined, "\0", start, true)
        result[#result + 1] = sub(joined, start, (stop or #joined + 1) - 1)
        start = (stop or #joined) + 1
    end
    sort(result)
    return result
end

local function clear_namespace(namespace)
    check_name(namespace, "argument #1 to 'clear'", 2)
    clear(namespace)
end

sdk.persist = setmetatable({ keys = keys, clear = clear_namespace }, {
    __call = function(_, namespace) return open(namespace) end,
    __metatable = false
})
)LUA";

        bool installing_wrapper = false;

        // luaL_loadbuffer goes through the Lua guard's lua_loadx hook, which calls RegisterLuaApi again, hence the
        // flag.
        void InstallWrapper(void* lua_state) noexcept
        {
            if (installing_wrapper) return;
            if (find_table(lua_state, lua_registry_index, "_LOADED._G.skeetsdk", 4) != nullptr) return;
            get_field(lua_state, -1, "persist");
            const bool done = get_type(lua_state, -1) == lua_type_table;
            set_top(lua_state, -3);
            if (done) return;
            installing_wrapper = true;
            if (load_buffer(lua_state, wrapper_source, sizeof(wrapper_source) - 1, "=skeetsdk") == 0)
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
        get_field(lua_state, -1, "_persist_get");
        const bool registered = get_type(lua_state, -1) == lua_type_function;
        set_top(lua_state, -2);
        if (!registered)
        {
            push_cclosure(lua_state, &NativeGet, 0);
            set_field(lua_state, -2, "_persist_get");
            push_cclosure(lua_state, &NativeSet, 0);
            set_field(lua_state, -2, "_persist_set");
            push_cclosure(lua_state, &NativeKeys, 0);
            set_field(lua_state, -2, "_persist_keys");
            push_cclosure(lua_state, &NativeClear, 0);
            set_field(lua_state, -2, "_persist_clear");
        }
        set_top(lua_state, -2);
        InstallWrapper(lua_state);
    }
#else
    void RegisterLuaApi(void*) noexcept {}
#endif
}
