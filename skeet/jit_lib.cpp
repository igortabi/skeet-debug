#include "jit_lib.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace skeetsdk::jit_lib
{
#if defined(_M_IX86)
    namespace
    {
        // Lua C API in the mapped image (same addresses the Lua guard uses).
        constexpr std::uintptr_t lua_push_cclosure_address = 0x43432F73u;
        constexpr std::uintptr_t lua_set_field_address = 0x43435B37u;
        constexpr std::uintptr_t lua_push_string_address = 0x434370E4u;
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
        constexpr std::int32_t itype_true = -3;

        // LuaJIT internals (verified in the image).
        constexpr std::size_t state_global_offset = 0x08;          // lua_State -> global_State
        constexpr std::size_t jit_flags_offset = 0x1E4;            // J->flags, from global_State
        constexpr std::size_t jit_param_offset = 0x788;            // J->param[], from global_State
        constexpr std::uintptr_t init_hotcount_address = 0x4342FADAu; // lj_dispatch_init_hotcount(global_State*)
        constexpr std::array<std::uint8_t, 8> init_hotcount_prologue{ 0x57, 0x8B, 0x7C, 0x24, 0x08, 0x6A, 0x20, 0x59 };

        // lj_jit.h (2.1): optimization flags and levels, and the parameters in JIT_PARAMDEF order.
        constexpr std::uint32_t jit_f_opt = 0x00010000u;
        constexpr std::uint32_t jit_f_opt_mask = 0x0FFF0000u;
        constexpr const char* opt_names[] = { "fold", "cse", "dce", "fwd", "dse", "narrow", "loop", "abc", "sink", "fuse" };
        constexpr std::uint32_t opt_flag(int index) noexcept { return jit_f_opt << index; }
        constexpr std::uint32_t opt_level_1 = opt_flag(0) | opt_flag(1) | opt_flag(2);                  // fold cse dce
        constexpr std::uint32_t opt_level_2 = opt_level_1 | opt_flag(5) | opt_flag(6);                  // + narrow loop
        constexpr std::uint32_t opt_level_3 = opt_level_2 | opt_flag(3) | opt_flag(4) | opt_flag(7)
            | opt_flag(8) | opt_flag(9);                                                                  // + fwd dse abc sink fuse
        constexpr std::uint32_t opt_levels[] = { 0, opt_level_1, opt_level_2, opt_level_3 };
        constexpr const char* param_names[] = { "maxtrace", "maxrecord", "maxirconst", "maxside", "maxsnap",
            "minstitch", "hotloop", "hotexit", "tryside", "instunroll", "loopunroll", "callunroll", "recunroll",
            "sizemcode", "maxmcode" };
        constexpr int param_hotloop = 6;

        using LuaCFunction = int(__cdecl*)(void*);
        using PushCClosureFn = void(__cdecl*)(void*, LuaCFunction, int);
        using SetFieldFn = void(__cdecl*)(void*, int, const char*);
        using PushStringFn = void(__cdecl*)(void*, const char*);
        using ToLStringFn = const char*(__cdecl*)(void*, int, std::size_t*);
        using SetTopFn = void(__cdecl*)(void*, int);
        using FindTableFn = const char*(__cdecl*)(void*, int, const char*, int);
        using TypeFn = int(__cdecl*)(void*, int);
        using GetFieldFn = void(__cdecl*)(void*, int, const char*);
        using LoadBufferFn = int(__cdecl*)(void*, const char*, std::size_t, const char*);
        using PCallFn = int(__cdecl*)(void*, int, int, int);
        using InitHotcountFn = void(__cdecl*)(void*);

        const auto push_cclosure = reinterpret_cast<PushCClosureFn>(lua_push_cclosure_address);
        const auto set_field = reinterpret_cast<SetFieldFn>(lua_set_field_address);
        const auto push_string = reinterpret_cast<PushStringFn>(lua_push_string_address);
        const auto to_lstring = reinterpret_cast<ToLStringFn>(lua_to_lstring_address);
        const auto set_top = reinterpret_cast<SetTopFn>(lua_set_top_address);
        const auto find_table = reinterpret_cast<FindTableFn>(lua_find_table_address);
        const auto get_type = reinterpret_cast<TypeFn>(lua_type_address);
        const auto get_field = reinterpret_cast<GetFieldFn>(lua_get_field_address);
        const auto load_buffer = reinterpret_cast<LoadBufferFn>(lua_load_buffer_address);
        const auto call_protected = reinterpret_cast<PCallFn>(lua_pcall_address);

        struct TValue
        {
            std::uint32_t value;
            std::int32_t type;
        };

        int StackSize(void* lua_state) noexcept
        {
            const auto state = static_cast<const std::uint8_t*>(lua_state);
            const auto base = *reinterpret_cast<TValue* const*>(state + 0x10);
            const auto top = *reinterpret_cast<TValue* const*>(state + 0x14);
            return base != nullptr && top >= base ? static_cast<int>(top - base) : 0;
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

        std::uint8_t* GlobalState(void* lua_state) noexcept
        {
            return *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(lua_state) + state_global_offset);
        }

        bool IsCode(std::uintptr_t address, const std::uint8_t* expected, std::size_t size) noexcept
        {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == 0) return false;
            if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
            return std::memcmp(reinterpret_cast<const void*>(address), expected, size) == 0;
        }

        // jitopt_level: "0" to "3".
        bool SetLevel(std::uint32_t& flags, std::string_view option) noexcept
        {
            if (option.size() != 1 || option[0] < '0' || option[0] > '9') return false;
            const int level = option[0] - '0';
            if (level > 3) return false;
            flags = (flags & ~jit_f_opt_mask) | opt_levels[level];
            return true;
        }

        // jitopt_flag: "name", "+name", "-name", "noname", "no-name".
        bool SetFlag(std::uint32_t& flags, std::string_view option) noexcept
        {
            bool set = true;
            if (!option.empty() && option[0] == '+') option.remove_prefix(1);
            else if (!option.empty() && option[0] == '-') { option.remove_prefix(1); set = false; }
            else if (option.size() >= 2 && option[0] == 'n' && option[1] == 'o')
            {
                option.remove_prefix(option.size() >= 3 && option[2] == '-' ? 3 : 2);
                set = false;
            }
            for (int i = 0; i < static_cast<int>(std::size(opt_names)); ++i)
            {
                if (option != opt_names[i]) continue;
                flags = set ? (flags | opt_flag(i)) : (flags & ~opt_flag(i));
                return true;
            }
            return false;
        }

        // jitopt_param: "name=value" with a whole number.
        bool SetParam(std::uint8_t* global_state, std::string_view option) noexcept
        {
            const std::size_t equals = option.find('=');
            if (equals == std::string_view::npos) return false;
            const std::string_view name = option.substr(0, equals);
            for (int i = 0; i < static_cast<int>(std::size(param_names)); ++i)
            {
                if (name != param_names[i]) continue;
                const std::string text(option.substr(equals + 1));
                if (text.empty()) return false;
                char* end = nullptr;
                errno = 0;
                const long value = std::strtol(text.c_str(), &end, 10);
                if (end == nullptr || *end != '\0' || errno == ERANGE || value < INT32_MIN || value > INT32_MAX) return false;
                reinterpret_cast<std::int32_t*>(global_state + jit_param_offset)[i] = static_cast<std::int32_t>(value);
                if (i == param_hotloop && IsCode(init_hotcount_address, init_hotcount_prologue.data(), init_hotcount_prologue.size()))
                    reinterpret_cast<InitHotcountFn>(init_hotcount_address)(global_state);
                return true;
            }
            return false;
        }

        // skeetsdk._jit_opt_start(...) -> true, or nil and the option that is not understood
        int __cdecl NativeOptStart(void* lua_state) noexcept
        {
            std::uint8_t* global_state = GlobalState(lua_state);
            auto& flags = *reinterpret_cast<std::uint32_t*>(global_state + jit_flags_offset);
            const int count = StackSize(lua_state);
            if (count == 0)
            {
                flags = (flags & ~jit_f_opt_mask) | opt_level_3;
                return ReturnTrue(lua_state);
            }
            for (int i = 1; i <= count; ++i)
            {
                const int type = get_type(lua_state, i);
                if (type != lua_type_string && type != lua_type_number)
                    return Fail(lua_state, "bad argument to 'start' (string expected)");
                std::size_t size = 0;
                const char* text = to_lstring(lua_state, i, &size);
                const std::string_view option(text != nullptr ? text : "", text != nullptr ? size : 0);
                if (!SetLevel(flags, option) && !SetFlag(flags, option) && !SetParam(global_state, option))
                {
                    try
                    {
                        static thread_local std::string message;
                        message = "unknown or malformed optimization flag '" + std::string(option) + "'";
                        return Fail(lua_state, message.c_str());
                    }
                    catch (...)
                    {
                        return Fail(lua_state, "unknown or malformed optimization flag");
                    }
                }
            }
            return ReturnTrue(lua_state);
        }

        constexpr char wrapper_source[] = R"LUA(
local sdk = skeetsdk
local jit = rawget(_G, "jit")
if type(jit) ~= "table" or type(jit.opt) == "table" then return end
local start, error, tostring = sdk._jit_opt_start, error, tostring
jit.version = "LuaJIT 2.1.0-beta3"
jit.version_num = 20100
jit.os = "Windows"
jit.arch = "x86"
jit.opt = {
    start = function(...)
        local ok, message = start(...)
        if not ok then error(tostring(message), 2) end
    end
}
if type(package) == "table" and type(package.loaded) == "table" then
    package.loaded["jit.opt"] = jit.opt
    if package.loaded.jit == nil then package.loaded.jit = jit end
end
)LUA";

        bool installing_wrapper = false;

        // luaL_loadbuffer goes through the Lua guard's lua_loadx hook, which calls RegisterLuaApi again, hence the
        // flag.
        void InstallWrapper(void* lua_state) noexcept
        {
            if (installing_wrapper) return;
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
        // Runs on every load and the GC is off, so do not allocate the closure again every time.
        get_field(lua_state, -1, "_jit_opt_start");
        const bool registered = get_type(lua_state, -1) == lua_type_function;
        set_top(lua_state, -2);
        if (!registered)
        {
            push_cclosure(lua_state, &NativeOptStart, 0);
            set_field(lua_state, -2, "_jit_opt_start");
        }
        set_top(lua_state, -2);
        if (!registered) InstallWrapper(lua_state);
    }
#else
    void RegisterLuaApi(void*) noexcept {}
#endif
}
