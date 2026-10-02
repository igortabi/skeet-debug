#include "pch.h"


#include "lua_loadstring_guard.hpp"
#define DEBUG_DEBUG 0
#ifndef DEBUG_DEBUG
#define DEBUG_DEBUG 0
#endif

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
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>

namespace skeetsdk::lua_guard
{
#if defined(_M_IX86)
    namespace
    {
        constexpr int lua_type_none = -1;
        constexpr int lua_type_number = 3;
        constexpr int lua_type_string = 4;
        constexpr int lua_type_nil = 0;
        constexpr int lua_type_table = 5;
        constexpr int lua_type_function = 6;
        constexpr int lua_type_thread = 8;
        constexpr int lua_registry_index = -10000;
        constexpr std::uintptr_t lua_create_table_address = 0x43428607u;
        constexpr std::uintptr_t lua_load_address = 0x43433944u;
        constexpr std::uintptr_t lua_push_cclosure_address = 0x43432F73u;
        constexpr std::uintptr_t lua_set_field_address = 0x43435B37u;
        constexpr std::uintptr_t lua_push_string_address = 0x434370E4u;
        constexpr std::uintptr_t lua_set_top_address = 0x43426BDFu;
        constexpr std::uintptr_t lua_find_table_address = 0x43426632u;
        constexpr std::uintptr_t lua_get_stack_address = 0x4342D7E7u;
        constexpr std::uintptr_t lua_get_info_address = 0x434322D1u;
        constexpr std::uintptr_t lua_type_address = 0x4343654Au;
        constexpr std::uintptr_t lua_push_value_address = 0x4342CEA7u;
        constexpr std::uintptr_t lua_remove_address = 0x434321D5u;
        constexpr std::uintptr_t lua_push_number_address = 0x4343525Fu;
        constexpr std::uintptr_t lua_to_lstring_address = 0x43435597u;
        constexpr std::uintptr_t lua_push_lstring_address = 0x43426891u;
        constexpr std::uintptr_t lua_get_field_address = 0x4342EBA2u;
        constexpr std::uintptr_t lua_call_address = 0x43431F3Au;
        constexpr std::uintptr_t lj_dispatch_update_address = 0x43432534u;
        constexpr std::uintptr_t lj_debug_framepc_address = 0x4343B0D9u;
        constexpr std::uintptr_t lj_debug_varname_address = 0x4343B1CDu;
        constexpr std::uintptr_t lj_debug_uvname_address = 0x4342B1B5u;
        constexpr std::uintptr_t lj_gc_barrierf_address = 0x43431E72u;
        constexpr std::uintptr_t lj_trace_flushall_address = 0x4342A819u;
#if DEBUG_DEBUG
        constexpr std::uintptr_t lual_loadbuffer_address = 0x43426D2Fu;
#endif

        constexpr char default_info_options[] = "flnSu";
        constexpr char valid_info_options[] = "SlnufL";
        constexpr std::size_t max_info_options = 16;

        // Same limits as LuaJIT's luaL_traceback: first 12 frames, "...", last 10 frames.
        constexpr int traceback_head_levels = 12;
        constexpr int traceback_tail_levels = 10;

        // GCfuncC layout: ffid byte at +6 (0 = Lua, 1 = plain C, >1 = builtin), C pointer at +0x14.
        constexpr std::size_t function_ffid_offset = 0x06;
        constexpr std::size_t function_cfunction_offset = 0x14;
        constexpr std::uint8_t function_ffid_c = 1;
        constexpr std::uint8_t function_ffid_lua = 0;

        // lua_getlocal/lua_setlocal are stripped as well ("(*temporary)" is not in the image), so
        // debug_localname is rebuilt on debug_framepc and lj_debug_varname. GCfuncL->pc at +0x10,
        // GCproto sits right below the bytecode; lua_State->stack at +0x1C.
        constexpr std::size_t state_stack_offset = 0x1C;
        constexpr std::size_t function_pc_offset = 0x10;
        constexpr std::size_t proto_size = 0x40;
        constexpr std::size_t proto_numparams_offset = 0x06;
        constexpr std::size_t proto_flags_offset = 0x25;
        constexpr std::uint8_t proto_vararg = 0x02;
        constexpr std::uint32_t frame_type_mask = 7;
        constexpr std::uint32_t frame_type_vararg = 3;
        constexpr std::uint32_t no_bytecode_position = 0xFFFFFFFFu;

        // lua_getupvalue/lua_setupvalue are stripped too. GCfunc: nupvalues byte at +0x07, Lua
        // closures hold GCupval refs at +0x14, C closures hold TValues at +0x18. GCupval->v at +0x10.
        constexpr std::size_t function_nupvalues_offset = 0x07;
        constexpr std::size_t function_lua_upvalues_offset = 0x14;
        constexpr std::size_t function_c_upvalues_offset = 0x18;
        constexpr std::size_t upvalue_value_offset = 0x10;
        // GC header marked byte; white bits and black bit as in lj_gc.h.
        constexpr std::size_t gc_marked_offset = 0x04;
        constexpr std::uint8_t gc_white_bits = 0x03;
        constexpr std::uint8_t gc_black_bit = 0x04;

        // Internal type tags (itype) of a TValue; numbers are any tag below itype_number_max.
        constexpr std::int32_t itype_nil = -1;
        constexpr std::int32_t itype_false = -2;
        constexpr std::int32_t itype_true = -3;
        constexpr std::int32_t itype_light_userdata = -4;
        constexpr std::int32_t itype_thread = -7;
        constexpr std::int32_t itype_function = -9;
        constexpr std::int32_t itype_table = -12;
        constexpr std::int32_t itype_userdata = -13;
        constexpr std::uint32_t itype_number_max = 0xFFFFFFF2u;
        constexpr std::uint32_t basemt_number_index = 13;

        // Environment and metatable refs. GCfunc->env at +0x08, GCudata->env at +0x08 and
        // ->metatable at +0x10, GCtab->metatable at +0x10, lua_State->env at +0x24.
        constexpr std::size_t function_env_offset = 0x08;
        constexpr std::size_t userdata_env_offset = 0x08;
        constexpr std::size_t userdata_metatable_offset = 0x10;
        constexpr std::size_t table_metatable_offset = 0x10;
        constexpr std::size_t thread_env_offset = 0x24;
        // g->gcroot[] at +0xE8; per-type base metatables start at GCROOT_BASEMT (= MM__MAX = 22).
        constexpr std::size_t global_gcroot_offset = 0xE8;
        constexpr std::size_t gcroot_basemt = 22;

        // lua_sethook/gethook are stripped from the image, so the hook state is written directly.
        // lua_State->glref at +0x08; global_State fields below (verified against callhook/lj_dispatch_ins).
        constexpr std::size_t state_global_offset = 0x08;
        constexpr std::size_t global_hookmask_offset = 0x71;
        constexpr std::size_t global_hookcount_offset = 0xC0;
        constexpr std::size_t global_hookcstart_offset = 0xC4;
        constexpr std::size_t global_hookf_offset = 0xC8;
        constexpr std::size_t global_trace_state_offset = 0x228;
        constexpr std::uint32_t trace_state_active = 0x10;
        constexpr std::uint8_t hook_event_mask = 0x0F;
        constexpr int hook_mask_call = 1 << 0;
        constexpr int hook_mask_return = 1 << 1;
        constexpr int hook_mask_line = 1 << 2;
        constexpr int hook_mask_count = 1 << 3;
        // Registry slot holding the Lua hook function; LuaJIT hooks are global, not per thread.
        constexpr char hook_registry_key[] = "skeetsdk.debug.hook";

        // lua_loadx: push ebp | mov ebp, esp | and esp, -8 | sub esp, 78h
        constexpr std::array<std::uint8_t, 9> load_prologue = {
            0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x78
        };
        constexpr std::size_t load_patch_size = 6;
        constexpr std::array<std::uint8_t, 4> get_stack_prologue = {
            0x55, 0x8B, 0xEC, 0x51
        };
        constexpr std::array<std::uint8_t, 6> get_info_prologue = {
            0x6A, 0x00, 0xFF, 0x74, 0x24, 0x10
        };

        struct TValue
        {
            std::uint32_t value;
            std::int32_t type;
        };

        struct LuaDebug
        {
            int event;
            const char* name;
            const char* name_what;
            const char* what;
            const char* source;
            int current_line;
            int upvalue_count;
            int line_defined;
            int last_line_defined;
            char short_source[60];
            int frame_index;
            int parameter_count;
            int is_vararg;
        };

        static_assert(sizeof(TValue) == 8);
        static_assert(offsetof(LuaDebug, frame_index) == 0x60);
        static_assert(sizeof(LuaDebug) == 0x6C);

        using LuaCFunction = int(__cdecl*)(void* lua_state);
        using LoadFn = int(__cdecl*)(void* lua_state, void* reader, void* data,
            const char* chunk_name, const char* mode);
        using CreateTableFn = void(__cdecl*)(void* lua_state, int array_size, int record_size);
        using PushCClosureFn = void(__cdecl*)(void* lua_state, LuaCFunction function, int upvalues);
        using SetFieldFn = void(__cdecl*)(void* lua_state, int index, const char* key);
        using PushStringFn = void(__cdecl*)(void* lua_state, const char* value);
        using SetTopFn = void(__cdecl*)(void* lua_state, int index);
        using FindTableFn = const char* (__cdecl*)(void* lua_state, int index, const char* name, int size_hint);
        using GetStackFn = int(__cdecl*)(void* lua_state, int level, LuaDebug* debug);
        using GetInfoFn = int(__cdecl*)(void* lua_state, const char* options, LuaDebug* debug);
        using TypeFn = int(__cdecl*)(void* lua_state, int index);
        using PushValueFn = void(__cdecl*)(void* lua_state, int index);
        using RemoveFn = void(__cdecl*)(void* lua_state, int index);
        using PushNumberFn = void(__cdecl*)(void* lua_state, double value);
        using ToLStringFn = const char* (__cdecl*)(void* lua_state, int index, std::size_t* length);
        using PushLStringFn = void(__cdecl*)(void* lua_state, const char* value, std::size_t length);
        using GetFieldFn = void(__cdecl*)(void* lua_state, int index, const char* key);
        using CallFn = void(__cdecl*)(void* lua_state, int argument_count, int result_count);
        using DispatchUpdateFn = void(__cdecl*)(void* global_state);
        using LuaHook = void(__cdecl*)(void* lua_state, LuaDebug* debug);
        using FramePcFn = std::uint32_t(__cdecl*)(void* lua_state, const void* function, const TValue* next_frame);
        using VarNameFn = const char* (__cdecl*)(const void* proto, std::uint32_t pc, std::uint32_t slot);
        using UpvalueNameFn = const char* (__cdecl*)(const void* proto, std::uint32_t index);
        using BarrierFn = void(__cdecl*)(void* global_state, void* owner, void* value);
        using FlushAllFn = int(__cdecl*)(void* lua_state);
#if DEBUG_DEBUG
        using LoadBufferFn = int(__cdecl*)(void* lua_state, const char* buffer, std::size_t size, const char* name);
#endif

        struct InlineHook
        {
            std::uintptr_t target{};
            std::array<std::uint8_t, 8> original_bytes{};
            std::size_t patch_size{};
            std::uint8_t* trampoline{};
        };

        struct HookState
        {
            std::mutex mutex;
            InlineHook load_hook{};
            LoadFn original_load{};
            bool installed{};
        };

        HookState hook_state;

        const auto create_table = reinterpret_cast<CreateTableFn>(lua_create_table_address);
        const auto push_cclosure = reinterpret_cast<PushCClosureFn>(lua_push_cclosure_address);
        const auto set_field = reinterpret_cast<SetFieldFn>(lua_set_field_address);
        const auto push_string = reinterpret_cast<PushStringFn>(lua_push_string_address);
        const auto set_top = reinterpret_cast<SetTopFn>(lua_set_top_address);
        const auto find_table = reinterpret_cast<FindTableFn>(lua_find_table_address);
        const auto get_stack = reinterpret_cast<GetStackFn>(lua_get_stack_address);
        const auto get_info = reinterpret_cast<GetInfoFn>(lua_get_info_address);
        const auto get_type = reinterpret_cast<TypeFn>(lua_type_address);
        const auto push_value = reinterpret_cast<PushValueFn>(lua_push_value_address);
        const auto remove_index = reinterpret_cast<RemoveFn>(lua_remove_address);
        const auto push_number = reinterpret_cast<PushNumberFn>(lua_push_number_address);
        const auto to_lstring = reinterpret_cast<ToLStringFn>(lua_to_lstring_address);
        const auto push_lstring = reinterpret_cast<PushLStringFn>(lua_push_lstring_address);
        const auto get_field = reinterpret_cast<GetFieldFn>(lua_get_field_address);
        const auto call = reinterpret_cast<CallFn>(lua_call_address);
        const auto dispatch_update = reinterpret_cast<DispatchUpdateFn>(lj_dispatch_update_address);
        const auto frame_pc = reinterpret_cast<FramePcFn>(lj_debug_framepc_address);
        const auto var_name = reinterpret_cast<VarNameFn>(lj_debug_varname_address);
        const auto upvalue_name = reinterpret_cast<UpvalueNameFn>(lj_debug_uvname_address);
        const auto gc_barrier = reinterpret_cast<BarrierFn>(lj_gc_barrierf_address);
        const auto trace_flush_all = reinterpret_cast<FlushAllFn>(lj_trace_flushall_address);
#if DEBUG_DEBUG
        const auto load_buffer = reinterpret_cast<LoadBufferFn>(lual_loadbuffer_address);
#endif

        bool IsReadable(const void* address, std::size_t size) noexcept
        {
            if (address == nullptr || size == 0) return false;
            MEMORY_BASIC_INFORMATION memory{};
            if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory)) return false;
            if (memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
            const auto begin = reinterpret_cast<std::uintptr_t>(address);
            const auto region_begin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
            const auto region_end = region_begin + memory.RegionSize;
            return begin >= region_begin && begin <= region_end && size <= region_end - begin;
        }

        bool IsExpectedCode(const void* address, const std::uint8_t* expected, std::size_t size) noexcept
        {
            return IsReadable(address, size)
                && std::equal(expected, expected + size, static_cast<const std::uint8_t*>(address));
        }

        // lua_State: base at +0x10, top at +0x14. index is 1-based like the Lua API.
        const TValue* Argument(void* lua_state, int index) noexcept
        {
            if (index < 1 || !IsReadable(lua_state, 0x18)) return nullptr;
            const auto state = static_cast<const std::uint8_t*>(lua_state);
            const auto stack_base = *reinterpret_cast<TValue* const*>(state + 0x10);
            const auto stack_top = *reinterpret_cast<TValue* const*>(state + 0x14);
            if (stack_base == nullptr || stack_top - stack_base < index) return nullptr;
            const TValue* argument = stack_base + (index - 1);
            return IsReadable(argument, sizeof(TValue)) ? argument : nullptr;
        }

        const TValue* StackTop(void* lua_state) noexcept
        {
            const auto state = static_cast<const std::uint8_t*>(lua_state);
            return *reinterpret_cast<TValue* const*>(state + 0x14) - 1;
        }

        // luaL_checkint-style read: truncates, rejects non-finite and out-of-range numbers.
        bool ReadInteger(void* lua_state, int index, int& value) noexcept
        {
            const TValue* argument = Argument(lua_state, index);
            if (argument == nullptr) return false;
            double number = 0.0;
            std::memcpy(&number, argument, sizeof(number));
            if (!std::isfinite(number) || number < static_cast<double>(std::numeric_limits<int>::min())
                || number > static_cast<double>(std::numeric_limits<int>::max()))
                return false;
            value = static_cast<int>(number);
            return true;
        }

        bool ReadLevel(void* lua_state, int index, int& level) noexcept
        {
            const TValue* argument = Argument(lua_state, index);
            if (argument == nullptr) return false;
            double number = 0.0;
            std::memcpy(&number, argument, sizeof(number));
            if (!std::isfinite(number) || number < 0.0
                || number > static_cast<double>(std::numeric_limits<int>::max()))
                return false;
            level = static_cast<int>(number);
            return true;
        }

        void SetStringField(void* lua_state, const char* key, const char* value) noexcept
        {
            push_string(lua_state, value);
            set_field(lua_state, -2, key);
        }

        void SetNumberField(void* lua_state, const char* key, int value) noexcept
        {
            push_number(lua_state, static_cast<double>(value));
            set_field(lua_state, -2, key);
        }

        // Moves a value lua_getinfo pushed ('f' or 'L') from below the result table into it.
        void MoveIntoField(void* lua_state, const char* key) noexcept
        {
            push_value(lua_state, -2);
            remove_index(lua_state, -3);
            set_field(lua_state, -2, key);
        }

        bool HasOption(const char* options, char option) noexcept
        {
            return std::strchr(options, option) != nullptr;
        }

        // debug.getinfo(function | level [, what]) with Lua 5.1 fields. Bad arguments return nil.
        int __cdecl NativeGetInfo(void* lua_state) noexcept
        {
            const char* options = default_info_options;
            if (get_type(lua_state, 2) == lua_type_string)
                options = to_lstring(lua_state, 2, nullptr);

            const std::size_t option_count = std::strlen(options);
            if (option_count > max_info_options || std::strspn(options, valid_info_options) != option_count)
            {
                push_string(lua_state, nullptr);
                return 1;
            }

            LuaDebug debug{};
            const int argument_type = get_type(lua_state, 1);
            if (argument_type == lua_type_function)
            {
                char function_options[max_info_options + 2] = { '>' };
                std::memcpy(function_options + 1, options, option_count + 1);
                push_value(lua_state, 1);
                if (get_info(lua_state, function_options, &debug) == 0)
                {
                    push_string(lua_state, nullptr);
                    return 1;
                }
            }
            else
            {
                int level = 0;
                if (argument_type != lua_type_number || !ReadLevel(lua_state, 1, level)
                    || get_stack(lua_state, level, &debug) == 0
                    || get_info(lua_state, options, &debug) == 0)
                {
                    push_string(lua_state, nullptr);
                    return 1;
                }
            }

            create_table(lua_state, 0, 2);
            if (HasOption(options, 'S'))
            {
                SetStringField(lua_state, "source", debug.source);
                SetStringField(lua_state, "short_src", debug.short_source);
                SetNumberField(lua_state, "linedefined", debug.line_defined);
                SetNumberField(lua_state, "lastlinedefined", debug.last_line_defined);
                SetStringField(lua_state, "what", debug.what);
            }
            if (HasOption(options, 'l')) SetNumberField(lua_state, "currentline", debug.current_line);
            if (HasOption(options, 'u')) SetNumberField(lua_state, "nups", debug.upvalue_count);
            if (HasOption(options, 'n'))
            {
                SetStringField(lua_state, "name", debug.name);
                SetStringField(lua_state, "namewhat", debug.name_what);
            }
            if (HasOption(options, 'L')) MoveIntoField(lua_state, "activelines");
            if (HasOption(options, 'f')) MoveIntoField(lua_state, "func");
            return 1;
        }

        // First level at or after `level` without a stack frame. lua_getstack walks the frame chain
        // on every call, so gallop + binary search keeps deep (stack overflow) tracebacks fast.
        int StackEnd(void* thread, int level) noexcept
        {
            LuaDebug debug{};
            if (get_stack(thread, level, &debug) == 0) return level;
            int present = level;
            int step = 1;
            int missing = level + 1;
            while (get_stack(thread, missing, &debug) != 0)
            {
                present = missing;
                step *= 2;
                missing = present + step;
            }
            while (missing - present > 1)
            {
                const int middle = present + (missing - present) / 2;
                if (get_stack(thread, middle, &debug) != 0)
                    present = middle;
                else
                    missing = middle;
            }
            return missing;
        }

        // Optional leading thread argument: returns the thread to inspect and sets `argument` to the
        // number of slots it used. nullptr if the thread value cannot be read.
        void* ThreadArgument(void* lua_state, int& argument) noexcept
        {
            argument = 0;
            if (get_type(lua_state, 1) != lua_type_thread) return lua_state;
            const TValue* value = Argument(lua_state, 1);
            if (value == nullptr) return nullptr;
            argument = 1;
            return reinterpret_cast<void*>(static_cast<std::uintptr_t>(value->value));
        }

        // Appends one frame in luaL_traceback's format.
        void AppendFrame(std::string& traceback, void* thread, LuaDebug& debug)
        {
            if (get_info(thread, "Snlf", &debug) == 0)
            {
                traceback += "\n\t?";
                return;
            }
            const auto* function = reinterpret_cast<const std::uint8_t*>(
                static_cast<std::uintptr_t>(StackTop(thread)->value));
            set_top(thread, -2);

            const bool named = debug.name_what != nullptr && *debug.name_what != '\0';
            const std::uint8_t ffid = function[function_ffid_offset];
            traceback += "\n\t";
            if (ffid > function_ffid_c && !named)
            {
                traceback += "[builtin#";
                traceback += std::to_string(ffid);
                traceback += "]:";
            }
            else
            {
                traceback += debug.short_source;
                traceback += ':';
            }
            if (debug.current_line > 0)
            {
                traceback += std::to_string(debug.current_line);
                traceback += ':';
            }

            if (named)
            {
                traceback += " in function '";
                traceback += debug.name != nullptr ? debug.name : "?";
                traceback += '\'';
            }
            else if (*debug.what == 'm')
            {
                traceback += " in main chunk";
            }
            else if (*debug.what == 'C')
            {
                std::uint32_t address = 0;
                std::memcpy(&address, function + function_cfunction_offset, sizeof(address));
                char text[16]{};
                std::snprintf(text, sizeof(text), "0x%08x", address);
                traceback += " at ";
                traceback += text;
            }
            else
            {
                traceback += " in function <";
                traceback += debug.short_source;
                traceback += ':';
                traceback += std::to_string(debug.line_defined);
                traceback += '>';
            }
        }

        // debug.traceback([thread,] [message [, level]]) with LuaJIT semantics: a non-string
        // message is returned unchanged, level defaults to 1 (0 for another thread).
        int __cdecl NativeTraceback(void* lua_state) noexcept
        {
            int argument = 0;
            void* thread = ThreadArgument(lua_state, argument);
            if (thread == nullptr)
            {
                push_string(lua_state, nullptr);
                return 1;
            }

            std::size_t message_length = 0;
            const char* message = nullptr;
            const int message_type = get_type(lua_state, argument + 1);
            if (message_type == lua_type_string || message_type == lua_type_number)
            {
                message = to_lstring(lua_state, argument + 1, &message_length);
            }
            else if (message_type != lua_type_none)
            {
                push_value(lua_state, argument + 1);
                return 1;
            }

            int level = thread == lua_state ? 1 : 0;
            if (get_type(lua_state, argument + 2) == lua_type_number)
                ReadLevel(lua_state, argument + 2, level);

            try
            {
                std::string traceback;
                if (message != nullptr)
                {
                    traceback.append(message, message_length);
                    traceback += '\n';
                }
                traceback += "stack traceback:";

                const int end = StackEnd(thread, level);
                const bool elide = end - level > traceback_head_levels + traceback_tail_levels;
                for (int current = level; current < end; ++current)
                {
                    if (elide && current == level + traceback_head_levels)
                    {
                        traceback += "\n\t...";
                        current = end - traceback_tail_levels - 1;
                        continue;
                    }
                    LuaDebug debug{};
                    if (get_stack(thread, current, &debug) == 0) break;
                    AppendFrame(traceback, thread, debug);
                }
                push_lstring(lua_state, traceback.data(), traceback.size());
            }
            catch (...)
            {
                push_string(lua_state, nullptr);
            }
            return 1;
        }

        std::uint8_t* GlobalState(void* lua_state) noexcept
        {
            return *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(lua_state) + state_global_offset);
        }

        template <typename T>
        T& GlobalField(std::uint8_t* global_state, std::size_t offset) noexcept
        {
            return *reinterpret_cast<T*>(global_state + offset);
        }

        // Equivalent of LuaJIT's lua_sethook.
        void SetHook(void* lua_state, LuaHook hook, int mask, int count) noexcept
        {
            std::uint8_t* global_state = GlobalState(lua_state);
            mask &= hook_event_mask;
            if (hook == nullptr || mask == 0)
            {
                mask = 0;
                hook = nullptr;
            }
            GlobalField<LuaHook>(global_state, global_hookf_offset) = hook;
            GlobalField<std::int32_t>(global_state, global_hookcount_offset) = count;
            GlobalField<std::int32_t>(global_state, global_hookcstart_offset) = count;
            auto& hook_mask = GlobalField<std::uint8_t>(global_state, global_hookmask_offset);
            hook_mask = static_cast<std::uint8_t>((hook_mask & ~hook_event_mask) | mask);
            GlobalField<std::uint32_t>(global_state, global_trace_state_offset) &= ~trace_state_active;
            dispatch_update(global_state);
        }

        // Calls the registry hook as hook(event [, line]). Not noexcept: errors raised by the
        // Lua hook unwind through this frame, so it must not own anything with a destructor.
        void __cdecl DispatchHook(void* lua_state, LuaDebug* debug)
        {
            static constexpr const char* event_names[] = { "call", "return", "line", "count", "tail return" };
            get_field(lua_state, lua_registry_index, hook_registry_key);
            if (get_type(lua_state, -1) != lua_type_function)
            {
                set_top(lua_state, -2);
                return;
            }
            const bool known_event = debug->event >= 0 && debug->event < static_cast<int>(std::size(event_names));
            push_string(lua_state, known_event ? event_names[debug->event] : "?");
            if (debug->current_line >= 0)
                push_number(lua_state, static_cast<double>(debug->current_line));
            else
                push_string(lua_state, nullptr);
            call(lua_state, 2, 0);
        }

        // debug.sethook([thread,] [hook, mask [, count]]). Hooks are global in LuaJIT, so the
        // thread is accepted for compatibility only. A non-function hook turns hooks off.
        int __cdecl NativeSetHook(void* lua_state) noexcept
        {
            const int argument = get_type(lua_state, 1) == lua_type_thread ? 1 : 0;
            LuaHook hook = nullptr;
            int mask = 0;
            int count = 0;
            if (get_type(lua_state, argument + 1) == lua_type_function)
            {
                hook = &DispatchHook;
                if (get_type(lua_state, argument + 2) == lua_type_string)
                {
                    const char* mask_text = to_lstring(lua_state, argument + 2, nullptr);
                    if (HasOption(mask_text, 'c')) mask |= hook_mask_call;
                    if (HasOption(mask_text, 'r')) mask |= hook_mask_return;
                    if (HasOption(mask_text, 'l')) mask |= hook_mask_line;
                }
                if (get_type(lua_state, argument + 3) == lua_type_number)
                    ReadLevel(lua_state, argument + 3, count);
                if (count > 0) mask |= hook_mask_count;
                push_value(lua_state, argument + 1);
            }
            else
            {
                push_string(lua_state, nullptr);
            }
            set_field(lua_state, lua_registry_index, hook_registry_key);
            SetHook(lua_state, hook, mask, count);
            return 0;
        }

        // debug.gethook([thread]) -> hook, mask, count. A hook installed from C reports "external hook".
        int __cdecl NativeGetHook(void* lua_state) noexcept
        {
            std::uint8_t* global_state = GlobalState(lua_state);
            const LuaHook hook = GlobalField<LuaHook>(global_state, global_hookf_offset);
            if (hook != nullptr && hook != &DispatchHook)
                push_string(lua_state, "external hook");
            else
                get_field(lua_state, lua_registry_index, hook_registry_key);

            const int mask = GlobalField<std::uint8_t>(global_state, global_hookmask_offset) & hook_event_mask;
            char mask_text[4]{};
            std::size_t length = 0;
            if (mask & hook_mask_call) mask_text[length++] = 'c';
            if (mask & hook_mask_return) mask_text[length++] = 'r';
            if (mask & hook_mask_line) mask_text[length++] = 'l';
            push_string(lua_state, mask_text);
            push_number(lua_state, static_cast<double>(
                GlobalField<std::int32_t>(global_state, global_hookcstart_offset)));
            return 3;
        }

        TValue* PointerValue(const TValue& value) noexcept
        {
            return reinterpret_cast<TValue*>(static_cast<std::uintptr_t>(value.value));
        }

        const std::uint8_t* FunctionProto(const std::uint8_t* function) noexcept
        {
            return *reinterpret_cast<const std::uint8_t* const*>(function + function_pc_offset) - proto_size;
        }

        // LuaJIT's debug_localname: the name of local `n` in the frame `debug` describes, with
        // `slot` set to its stack slot. Negative `n` addresses varargs. nullptr if there is none.
        const char* LocalSlot(void* thread, const LuaDebug& debug, int n, TValue*& slot) noexcept
        {
            const auto state = static_cast<std::uint8_t*>(thread);
            const auto frame_index = static_cast<std::uint32_t>(debug.frame_index);
            TValue* frame = *reinterpret_cast<TValue**>(state + state_stack_offset) + (frame_index & 0xFFFF);
            TValue* next_frame = (frame_index >> 16) != 0 ? frame + (frame_index >> 16) : nullptr;
            const auto* function = reinterpret_cast<const std::uint8_t*>(PointerValue(*frame));
            const std::uint32_t pc = frame_pc(thread, function, next_frame);
            if (next_frame == nullptr) next_frame = *reinterpret_cast<TValue**>(state + 0x14);

            if (n < 0)
            {
                if (pc == no_bytecode_position) return nullptr;
                const std::uint8_t* proto = FunctionProto(function);
                if ((proto[proto_flags_offset] & proto_vararg) == 0) return nullptr;
                const std::ptrdiff_t index = proto[proto_numparams_offset] - static_cast<std::ptrdiff_t>(n);
                if ((static_cast<std::uint32_t>(frame->type) & frame_type_mask) == frame_type_vararg)
                {
                    next_frame = frame;
                    frame = reinterpret_cast<TValue*>(reinterpret_cast<std::uint8_t*>(frame)
                        - (static_cast<std::uint32_t>(frame->type) & ~frame_type_mask));
                }
                if (index >= next_frame - frame) return nullptr;
                slot = frame + index;
                return "(*vararg)";
            }

            const char* name = nullptr;
            if (pc != no_bytecode_position)
                name = var_name(FunctionProto(function), pc, static_cast<std::uint32_t>(n) - 1);
            if (name == nullptr && n > 0 && n < next_frame - frame) name = "(*temporary)";
            if (name != nullptr) slot = frame + n;
            return name;
        }

        // debug.getlocal([thread,] level, n) -> name, value, or nil when there is no such local.
        // debug.getlocal(function, n) -> name of parameter n of a Lua function.
        int __cdecl NativeGetLocal(void* lua_state) noexcept
        {
            int argument = 0;
            void* thread = ThreadArgument(lua_state, argument);
            int n = 0;
            if (thread == nullptr || get_type(lua_state, argument + 2) != lua_type_number
                || !ReadInteger(lua_state, argument + 2, n))
            {
                push_string(lua_state, nullptr);
                return 1;
            }

            if (get_type(lua_state, argument + 1) == lua_type_function)
            {
                const auto* function = reinterpret_cast<const std::uint8_t*>(
                    PointerValue(*Argument(lua_state, argument + 1)));
                const char* name = nullptr;
                if (function[function_ffid_offset] == function_ffid_lua)
                    name = var_name(FunctionProto(function), 0, static_cast<std::uint32_t>(n) - 1);
                push_string(lua_state, name);
                return 1;
            }

            int level = 0;
            LuaDebug debug{};
            if (get_type(lua_state, argument + 1) != lua_type_number || !ReadLevel(lua_state, argument + 1, level)
                || get_stack(thread, level, &debug) == 0)
            {
                push_string(lua_state, nullptr);
                return 1;
            }

            // Reserve the value slot first: pushing can reallocate the stack the local lives in.
            push_string(lua_state, nullptr);
            TValue* slot = nullptr;
            const char* name = LocalSlot(thread, debug, n, slot);
            if (name == nullptr) return 1;
            *const_cast<TValue*>(StackTop(lua_state)) = *slot;
            push_string(lua_state, name);
            push_value(lua_state, -2);
            return 2;
        }

        // debug.setlocal([thread,] level, n, value) -> name of the assigned local, or nil.
        int __cdecl NativeSetLocal(void* lua_state) noexcept
        {
            int argument = 0;
            void* thread = ThreadArgument(lua_state, argument);
            int level = 0;
            int n = 0;
            LuaDebug debug{};
            const TValue* value = Argument(lua_state, argument + 3);
            if (thread == nullptr || value == nullptr
                || get_type(lua_state, argument + 1) != lua_type_number || !ReadLevel(lua_state, argument + 1, level)
                || get_type(lua_state, argument + 2) != lua_type_number || !ReadInteger(lua_state, argument + 2, n)
                || get_stack(thread, level, &debug) == 0)
            {
                push_string(lua_state, nullptr);
                return 1;
            }

            TValue* slot = nullptr;
            const char* name = LocalSlot(thread, debug, n, slot);
            if (name != nullptr) *slot = *value;
            push_string(lua_state, name);
            return 1;
        }

        bool IsGcValue(const TValue& value) noexcept
        {
            return static_cast<std::uint32_t>(value.type) + 4u > 0xFFFFFFF6u;
        }

        // lj_gc_objbarrier: a black owner must not point at a white object. The forward barrier
        // is used for tables too; lj_gc_objbarriert's backward barrier is only an optimization.
        void ObjectBarrier(void* lua_state, std::uint8_t* owner, std::uint8_t* object) noexcept
        {
            if (object != nullptr && (object[gc_marked_offset] & gc_white_bits) != 0
                && (owner[gc_marked_offset] & gc_black_bit) != 0)
                gc_barrier(GlobalState(lua_state), owner, object);
        }

        // Pushes a raw TValue: reserve a slot with nil, then overwrite it.
        void PushRaw(void* lua_state, std::uint32_t value, std::int32_t type) noexcept
        {
            push_string(lua_state, nullptr);
            *const_cast<TValue*>(StackTop(lua_state)) = TValue{ value, type };
        }

        void PushTableOrNil(void* lua_state, const std::uint8_t* table) noexcept
        {
            if (table == nullptr)
                push_string(lua_state, nullptr);
            else
                PushRaw(lua_state, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(table)), itype_table);
        }

        std::uint8_t*& ObjectField(const TValue& value, std::size_t offset) noexcept
        {
            return *reinterpret_cast<std::uint8_t**>(reinterpret_cast<std::uint8_t*>(PointerValue(value)) + offset);
        }

        // basemt_obj: the metatable slot shared by all values of a non-table, non-userdata type.
        std::uint8_t*& BaseMetatable(void* lua_state, std::int32_t type) noexcept
        {
            const auto itype = static_cast<std::uint32_t>(type);
            const std::size_t index = itype < itype_number_max ? basemt_number_index : ~itype;
            return GlobalField<std::uint8_t*>(GlobalState(lua_state),
                global_gcroot_offset + (gcroot_basemt + index) * sizeof(std::uint32_t));
        }

        // lua_getfenv/lua_setfenv slot: functions, userdata and threads have an environment.
        std::uint8_t** EnvironmentSlot(const TValue& value) noexcept
        {
            switch (value.type)
            {
            case itype_function: return &ObjectField(value, function_env_offset);
            case itype_userdata: return &ObjectField(value, userdata_env_offset);
            case itype_thread: return &ObjectField(value, thread_env_offset);
            default: return nullptr;
            }
        }

        // debug.getmetatable(value) -> metatable or nil, ignoring __metatable.
        int __cdecl NativeGetMetatable(void* lua_state) noexcept
        {
            const TValue* value = Argument(lua_state, 1);
            if (value == nullptr)
            {
                push_string(lua_state, nullptr);
                return 1;
            }
            const std::uint8_t* metatable = nullptr;
            if (value->type == itype_table)
                metatable = ObjectField(*value, table_metatable_offset);
            else if (value->type == itype_userdata)
                metatable = ObjectField(*value, userdata_metatable_offset);
            else
                metatable = BaseMetatable(lua_state, value->type);
            PushTableOrNil(lua_state, metatable);
            return 1;
        }

        // debug.setmetatable(value, table | nil) -> value. Works on any type and ignores
        // __metatable; non-table, non-userdata types share one metatable per type.
        // Returns nil if the metatable is not nil or a table, or while a __gc finalizer runs.
        int __cdecl NativeSetMetatable(void* lua_state) noexcept
        {
            const int metatable_type = get_type(lua_state, 2);
            if (Argument(lua_state, 1) == nullptr || (metatable_type != lua_type_table
                && metatable_type != lua_type_nil && metatable_type != lua_type_none))
            {
                push_string(lua_state, nullptr);
                return 1;
            }
            const TValue* metatable_value = metatable_type == lua_type_table ? Argument(lua_state, 2) : nullptr;
            auto* metatable = metatable_value != nullptr
                ? reinterpret_cast<std::uint8_t*>(PointerValue(*metatable_value)) : nullptr;

            const TValue& value = *Argument(lua_state, 1);
            if (value.type == itype_table || value.type == itype_userdata)
            {
                ObjectField(value, value.type == itype_table ? table_metatable_offset : userdata_metatable_offset) = metatable;
                ObjectBarrier(lua_state, reinterpret_cast<std::uint8_t*>(PointerValue(value)), metatable);
            }
            else
            {
                // Traces specialize to base metatables, so flush them first. No barrier needed:
                // base metatables are GC roots.
                if (trace_flush_all(lua_state) != 0)
                {
                    push_string(lua_state, nullptr);
                    return 1;
                }
                const std::int32_t type = Argument(lua_state, 1)->type;
                if (type == itype_false || type == itype_true)
                {
                    BaseMetatable(lua_state, itype_false) = metatable;
                    BaseMetatable(lua_state, itype_true) = metatable;
                }
                else
                {
                    BaseMetatable(lua_state, type) = metatable;
                }
            }
            push_value(lua_state, 1);
            return 1;
        }

        // debug.getfenv(value) -> environment of a function, userdata or thread, else nil.
        int __cdecl NativeGetFenv(void* lua_state) noexcept
        {
            const TValue* value = Argument(lua_state, 1);
            std::uint8_t** slot = value != nullptr ? EnvironmentSlot(*value) : nullptr;
            PushTableOrNil(lua_state, slot != nullptr ? *slot : nullptr);
            return 1;
        }

        // debug.setfenv(value, table) -> value. nil if value has no environment or table is not a table.
        int __cdecl NativeSetFenv(void* lua_state) noexcept
        {
            const TValue* value = Argument(lua_state, 1);
            std::uint8_t** slot = value != nullptr ? EnvironmentSlot(*value) : nullptr;
            if (slot == nullptr || get_type(lua_state, 2) != lua_type_table)
            {
                push_string(lua_state, nullptr);
                return 1;
            }
            auto* environment = reinterpret_cast<std::uint8_t*>(PointerValue(*Argument(lua_state, 2)));
            *slot = environment;
            ObjectBarrier(lua_state, reinterpret_cast<std::uint8_t*>(PointerValue(*value)), environment);
            push_value(lua_state, 1);
            return 1;
        }

        // debug.getregistry() -> the registry table.
        int __cdecl NativeGetRegistry(void* lua_state) noexcept
        {
            push_value(lua_state, lua_registry_index);
            return 1;
        }

        // lj_debug_uvnamev: name of upvalue `n` (1-based) with `slot` set to its value and `owner` to
        // the GC object that holds it (the GCupval, or the closure itself for C functions).
        const char* UpvalueSlot(std::uint8_t* function, int n, TValue*& slot, std::uint8_t*& owner) noexcept
        {
            if (n < 1 || n > function[function_nupvalues_offset]) return nullptr;
            const auto index = static_cast<std::uint32_t>(n - 1);
            if (function[function_ffid_offset] == function_ffid_lua)
            {
                owner = reinterpret_cast<std::uint8_t**>(function + function_lua_upvalues_offset)[index];
                slot = *reinterpret_cast<TValue**>(owner + upvalue_value_offset);
                return upvalue_name(FunctionProto(function), index);
            }
            owner = function;
            slot = reinterpret_cast<TValue*>(function + function_c_upvalues_offset) + index;
            return "";
        }

        std::uint8_t* FunctionArgument(void* lua_state, int index) noexcept
        {
            if (get_type(lua_state, index) != lua_type_function) return nullptr;
            return reinterpret_cast<std::uint8_t*>(PointerValue(*Argument(lua_state, index)));
        }

        // debug.getupvalue(f, n) -> name, value; nothing when f has no upvalue n.
        int __cdecl NativeGetUpvalue(void* lua_state) noexcept
        {
            std::uint8_t* function = FunctionArgument(lua_state, 1);
            int n = 0;
            TValue* slot = nullptr;
            std::uint8_t* owner = nullptr;
            if (function == nullptr || get_type(lua_state, 2) != lua_type_number
                || !ReadInteger(lua_state, 2, n))
                return 0;
            const char* name = UpvalueSlot(function, n, slot, owner);
            if (name == nullptr) return 0;
            push_string(lua_state, name);
            push_string(lua_state, nullptr);
            // An open upvalue points into a stack, which the pushes above may have reallocated.
            UpvalueSlot(function, n, slot, owner);
            *const_cast<TValue*>(StackTop(lua_state)) = *slot;
            return 2;
        }

        // debug.setupvalue(f, n, value) -> name; nothing when f has no upvalue n.
        int __cdecl NativeSetUpvalue(void* lua_state) noexcept
        {
            std::uint8_t* function = FunctionArgument(lua_state, 1);
            const TValue* value = Argument(lua_state, 3);
            int n = 0;
            TValue* slot = nullptr;
            std::uint8_t* owner = nullptr;
            if (function == nullptr || value == nullptr || get_type(lua_state, 2) != lua_type_number
                || !ReadInteger(lua_state, 2, n))
                return 0;
            const char* name = UpvalueSlot(function, n, slot, owner);
            if (name == nullptr) return 0;
            *slot = *value;
            if (IsGcValue(*value))
                ObjectBarrier(lua_state, owner, reinterpret_cast<std::uint8_t*>(PointerValue(*value)));
            push_string(lua_state, name);
            return 1;
        }

        // Upvalue reference cell n (1-based) of a Lua function, or nullptr.
        std::uint8_t** LuaUpvalueRef(void* lua_state, int function_index, int n_index) noexcept
        {
            std::uint8_t* function = FunctionArgument(lua_state, function_index);
            int n = 0;
            if (function == nullptr || function[function_ffid_offset] != function_ffid_lua
                || get_type(lua_state, n_index) != lua_type_number || !ReadInteger(lua_state, n_index, n)
                || n < 1 || n > function[function_nupvalues_offset])
                return nullptr;
            return reinterpret_cast<std::uint8_t**>(function + function_lua_upvalues_offset) + (n - 1);
        }

        // debug.upvalueid(f, n) -> light userdata identifying the upvalue, or nil.
        int __cdecl NativeUpvalueId(void* lua_state) noexcept
        {
            std::uint8_t* function = FunctionArgument(lua_state, 1);
            int n = 0;
            TValue* slot = nullptr;
            std::uint8_t* owner = nullptr;
            if (function == nullptr || get_type(lua_state, 2) != lua_type_number || !ReadInteger(lua_state, 2, n)
                || UpvalueSlot(function, n, slot, owner) == nullptr)
            {
                push_string(lua_state, nullptr);
                return 1;
            }
            // The GCupval for Lua functions, the TValue inside the closure for C functions.
            const void* id = function[function_ffid_offset] == function_ffid_lua
                ? static_cast<const void*>(owner) : static_cast<const void*>(slot);
            PushRaw(lua_state, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(id)), itype_light_userdata);
            return 1;
        }

        // debug.upvaluejoin(f1, n1, f2, n2): makes upvalue n1 of Lua function f1 refer to upvalue
        // n2 of Lua function f2. Does nothing on bad arguments.
        int __cdecl NativeUpvalueJoin(void* lua_state) noexcept
        {
            std::uint8_t** target = LuaUpvalueRef(lua_state, 1, 2);
            std::uint8_t** source = LuaUpvalueRef(lua_state, 3, 4);
            if (target == nullptr || source == nullptr) return 0;
            *target = *source;
            ObjectBarrier(lua_state, FunctionArgument(lua_state, 1), *source);
            return 0;
        }

#if DEBUG_DEBUG
        // Console for debug.debug. The game has none, so one is allocated on first use and freed
        // again on "cont". Its close button is removed: closing a console window kills the process.
        class DebugConsole
        {
        public:
            DebugConsole() noexcept
            {
                allocated_ = AllocConsole() != FALSE;
                if (HWND window = GetConsoleWindow())
                {
                    DeleteMenu(GetSystemMenu(window, FALSE), SC_CLOSE, MF_BYCOMMAND);
                    SetForegroundWindow(window);
                }
                SetConsoleCtrlHandler(&IgnoreControl, TRUE);
                input_ = CreateFileA("CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING, 0, nullptr);
                output_ = CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING, 0, nullptr);
            }

            ~DebugConsole()
            {
                if (input_ != INVALID_HANDLE_VALUE) CloseHandle(input_);
                if (output_ != INVALID_HANDLE_VALUE) CloseHandle(output_);
                SetConsoleCtrlHandler(&IgnoreControl, FALSE);
                if (allocated_) FreeConsole();
            }

            DebugConsole(const DebugConsole&) = delete;
            DebugConsole& operator=(const DebugConsole&) = delete;

            bool IsOpen() const noexcept
            {
                return input_ != INVALID_HANDLE_VALUE && output_ != INVALID_HANDLE_VALUE;
            }

            void Write(const char* text) const noexcept
            {
                DWORD written = 0;
                WriteConsoleA(output_, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
            }

            // Reads one line without its line ending. false on failure.
            bool ReadLine(char* buffer, std::size_t size) const noexcept
            {
                DWORD read = 0;
                if (!ReadConsoleA(input_, buffer, static_cast<DWORD>(size - 1), &read, nullptr) || read == 0)
                    return false;
                buffer[read] = '\0';
                buffer[std::strcspn(buffer, "\r\n")] = '\0';
                return true;
            }

        private:
            static BOOL WINAPI IgnoreControl(DWORD) noexcept { return TRUE; }

            bool allocated_{};
            HANDLE input_{ INVALID_HANDLE_VALUE };
            HANDLE output_{ INVALID_HANDLE_VALUE };
        };

        // Environment of the Lua function at `level`, or nullptr.
        std::uint8_t* LevelEnvironment(void* lua_state, int level) noexcept
        {
            LuaDebug debug{};
            if (get_stack(lua_state, level, &debug) == 0 || get_info(lua_state, "f", &debug) == 0)
                return nullptr;
            const TValue* function = StackTop(lua_state);
            std::uint8_t** slot = EnvironmentSlot(*function);
            std::uint8_t* environment = slot != nullptr ? *slot : nullptr;
            set_top(lua_state, -2);
            return environment;
        }

        // Runs one debug.debug command through the real _G.pcall, so errors are caught without
        // lua_pcall (stripped from the image). Commands see the caller's environment.
        void RunDebugCommand(void* lua_state, const DebugConsole& console, const char* line,
            std::uint8_t* environment)
        {
            if (load_buffer(lua_state, line, std::strlen(line), "=(debug command)") == 0)
            {
                if (environment != nullptr)
                {
                    auto* chunk = reinterpret_cast<std::uint8_t*>(PointerValue(*StackTop(lua_state)));
                    *reinterpret_cast<std::uint8_t**>(chunk + function_env_offset) = environment;
                    ObjectBarrier(lua_state, chunk, environment);
                }
                if (find_table(lua_state, lua_registry_index, "_LOADED._G", 1) != nullptr)
                {
                    set_top(lua_state, -2);
                    console.Write("(debug.debug: _G not found)\n");
                    return;
                }
                get_field(lua_state, -1, "pcall");
                remove_index(lua_state, -2);
                push_value(lua_state, -2);
                call(lua_state, 1, 2);
                // Stack: chunk, ok, error. Only a false `ok` leaves a message to print.
                if (StackTop(lua_state)[-1].type != itype_false) return;
            }
            const char* message = to_lstring(lua_state, -1, nullptr);
            console.Write(message != nullptr ? message : "(error object is not a string)");
            console.Write("\n");
        }

        // debug.debug(): interactive prompt in a console window until "cont". Blocks the calling
        // (game) thread while waiting for input, like the stock version blocks on stdin.
        int __cdecl NativeDebug(void* lua_state)
        {
            DebugConsole console;
            if (!console.IsOpen()) return 0;
            std::uint8_t* environment = LevelEnvironment(lua_state, 1);
            for (;;)
            {
                char line[250]{};
                console.Write("lua_debug> ");
                if (!console.ReadLine(line, sizeof(line)) || std::strcmp(line, "cont") == 0)
                    return 0;
                RunDebugCommand(lua_state, console, line, environment);
                set_top(lua_state, 0);
            }
        }
#endif

        // Runs on every load instead of once per lua_State: a recreated state can reuse the old
        // address. Registers through registry._LOADED._G (the real _G) because scripts load while
        // setfenv(0, sandbox) is active, and the sandboxes fall back to _G via __index.
        // package.loaded.debug points at the same table so require("debug") returns it.
        void RegisterDebug(void* lua_state) noexcept
        {
            if (find_table(lua_state, lua_registry_index, "_LOADED._G.debug", 2) != nullptr) return;
            push_cclosure(lua_state, &NativeGetInfo, 0);
            set_field(lua_state, -2, "getinfo");
            push_cclosure(lua_state, &NativeTraceback, 0);
            set_field(lua_state, -2, "traceback");
            push_cclosure(lua_state, &NativeSetHook, 0);
            set_field(lua_state, -2, "sethook");
            push_cclosure(lua_state, &NativeGetHook, 0);
            set_field(lua_state, -2, "gethook");
            push_cclosure(lua_state, &NativeGetLocal, 0);
            set_field(lua_state, -2, "getlocal");
            push_cclosure(lua_state, &NativeSetLocal, 0);
            set_field(lua_state, -2, "setlocal");
            push_cclosure(lua_state, &NativeGetUpvalue, 0);
            set_field(lua_state, -2, "getupvalue");
            push_cclosure(lua_state, &NativeSetUpvalue, 0);
            set_field(lua_state, -2, "setupvalue");
            push_cclosure(lua_state, &NativeUpvalueId, 0);
            set_field(lua_state, -2, "upvalueid");
            push_cclosure(lua_state, &NativeUpvalueJoin, 0);
            set_field(lua_state, -2, "upvaluejoin");
            push_cclosure(lua_state, &NativeGetMetatable, 0);
            set_field(lua_state, -2, "getmetatable");
            push_cclosure(lua_state, &NativeSetMetatable, 0);
            set_field(lua_state, -2, "setmetatable");
            push_cclosure(lua_state, &NativeGetFenv, 0);
            set_field(lua_state, -2, "getfenv");
            push_cclosure(lua_state, &NativeSetFenv, 0);
            set_field(lua_state, -2, "setfenv");
            push_cclosure(lua_state, &NativeGetRegistry, 0);
            set_field(lua_state, -2, "getregistry");
#if DEBUG_DEBUG
            push_cclosure(lua_state, &NativeDebug, 0);
            set_field(lua_state, -2, "debug");
#endif
            if (find_table(lua_state, lua_registry_index, "_LOADED", 16) == nullptr)
            {
                push_value(lua_state, -2);
                set_field(lua_state, -2, "debug");
                set_top(lua_state, -2);
            }
            set_top(lua_state, -2);
        }

        int __cdecl GuardedLoad(void* lua_state, void* reader, void* data,
            const char* chunk_name, const char* mode) noexcept
        {
            RegisterDebug(lua_state);
            const LoadFn original = hook_state.original_load;
            return original != nullptr ? original(lua_state, reader, data, chunk_name, mode) : 1;
        }

        void WriteJump(std::uint8_t* source, const void* destination) noexcept
        {
            const auto source_after_jump = reinterpret_cast<std::uintptr_t>(source + 5);
            const auto target = reinterpret_cast<std::uintptr_t>(destination);
            source[0] = 0xE9;
            *reinterpret_cast<std::uint32_t*>(source + 1) = static_cast<std::uint32_t>(target - source_after_jump);
        }
    }

    bool Install(std::uintptr_t target) noexcept
    {
        std::scoped_lock lock(hook_state.mutex);
        if (hook_state.installed) return hook_state.load_hook.target == target;

        auto install_one = [](InlineHook& hook, std::uintptr_t address, const void* replacement) noexcept
            {
                const std::size_t patch_size = address == lua_load_address ? load_patch_size : 0;
                auto* target_bytes = reinterpret_cast<std::uint8_t*>(address);
                if (patch_size == 0 || !IsExpectedCode(target_bytes, load_prologue.data(), load_prologue.size())) return false;
                auto* trampoline = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, patch_size + 5,
                    MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
                if (trampoline == nullptr) return false;
                std::memcpy(hook.original_bytes.data(), target_bytes, patch_size);
                std::memcpy(trampoline, target_bytes, patch_size);
                WriteJump(trampoline + patch_size, target_bytes + patch_size);
                DWORD old_protection{};
                if (!VirtualProtect(target_bytes, patch_size, PAGE_EXECUTE_READWRITE, &old_protection))
                {
                    VirtualFree(trampoline, 0, MEM_RELEASE);
                    return false;
                }
                WriteJump(target_bytes, replacement);
                if (patch_size > 5) std::memset(target_bytes + 5, 0x90, patch_size - 5);
                DWORD ignored{};
                VirtualProtect(target_bytes, patch_size, old_protection, &ignored);
                FlushInstructionCache(GetCurrentProcess(), target_bytes, patch_size);
                hook.target = address;
                hook.patch_size = patch_size;
                hook.trampoline = trampoline;
                return true;
            };

        if (!install_one(hook_state.load_hook, target, reinterpret_cast<const void*>(&GuardedLoad))) return false;
        hook_state.original_load = reinterpret_cast<LoadFn>(hook_state.load_hook.trampoline);
        hook_state.installed = true;
        return true;
    }

    void Remove() noexcept
    {
        std::scoped_lock lock(hook_state.mutex);
        if (!hook_state.installed) return;
        auto remove_one = [](InlineHook& hook) noexcept
            {
                if (hook.target == 0) return;
                auto* target = reinterpret_cast<std::uint8_t*>(hook.target);
                DWORD old_protection{};
                if (VirtualProtect(target, hook.patch_size, PAGE_EXECUTE_READWRITE, &old_protection))
                {
                    std::memcpy(target, hook.original_bytes.data(), hook.patch_size);
                    DWORD ignored{};
                    VirtualProtect(target, hook.patch_size, old_protection, &ignored);
                    FlushInstructionCache(GetCurrentProcess(), target, hook.patch_size);
                }
                if (hook.trampoline != nullptr) VirtualFree(hook.trampoline, 0, MEM_RELEASE);
                hook = {};
            };
        remove_one(hook_state.load_hook);
        hook_state.original_load = nullptr;
        hook_state.installed = false;
    }

    bool IsInstalled() noexcept
    {
        std::scoped_lock lock(hook_state.mutex);
        return hook_state.installed;
    }
#else
    bool Install(std::uintptr_t) noexcept { return false; }
    void Remove() noexcept {}
    bool IsInstalled() noexcept { return false; }
#endif
}
