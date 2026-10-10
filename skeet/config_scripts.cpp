#include "config_scripts.hpp"

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
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// A config is a blob: {signature, version, key} followed by units {u16 size, u16 type, u32 hash,
// data}. The game finds a unit by hash and gives up at the first unit whose type is 8, so a unit of
// our own goes at the very end of the blob where nothing else ever looks.
//
//  Save button: the serializer is hooked for the duration of the button handler and appends a unit
//               with the names of the loaded scripts.
//  Load button: _LoadConfig is hooked for the duration of the button handler and reads that unit
//               back. Before the original applies any values, scripts the config does not list are
//               unloaded and the ones it lists are loaded, through the same routines the
//               Load/Unload script buttons use. The values are only applied to controls that exist
//               at that moment (loading a script does not restore its saved values), so the
//               scripts have to be loaded first or they would come up with default values.
//
// Only the buttons are hooked into, not config.load(): a script calling config.load() must not
// unload itself while it is still running.

namespace skeetsdk::config_scripts
{
#if defined(_M_IX86)
    namespace
    {
        // Config tab handlers and config routines in the mapped image (verified in the IDB).
        constexpr std::uintptr_t save_handler_address = 0x433B5919u;      // Save button, __thiscall(tab, arg)
        constexpr std::uintptr_t load_handler_address = 0x43383015u;      // Load button, __thiscall(tab, arg)
        constexpr std::uintptr_t serializer_address = 0x433D0EB1u;        // __fastcall(defaults, std::vector<u8>*)
        constexpr std::uintptr_t load_config_address = 0x433ED806u;       // __fastcall(menu, data, size)
        // Script routines the Load/Unload script buttons are made of.
        constexpr std::uintptr_t load_lua_address = 0x43419B27u;          // __thiscall(const wchar_t* name) -> bool
        constexpr std::uintptr_t unload_lua_address = 0x433DF341u;        // __thiscall(const wchar_t* name) -> bool
        constexpr std::uintptr_t is_lua_loaded_address = 0x43347065u;     // __thiscall(name vector*) -> bool
        constexpr std::uintptr_t update_lua_info_address = 0x433F1A21u;   // __thiscall(config tab)
        constexpr std::uintptr_t lua_slot_address = 0x433CBCBBu;          // __cdecl() -> {lua_State*, ...}* or null
        constexpr std::uintptr_t resize_vector_address = 0x433E1218u;     // __thiscall(std::vector<u8>*, new size)
        // Only for the diagnostics below.
        constexpr std::uintptr_t find_unit_address = 0x43367569u;         // __fastcall(blob, size, node) -> unit*
        constexpr std::uintptr_t lua_records_address = 0x43479760u;       // {begin, end} of the script controls (28 bytes each)
        constexpr std::uintptr_t value_offset_table_address = 0x43472DCCu; // node value offset per type, 0xFF = none
        constexpr std::size_t lua_record_size = 28;
        constexpr std::uintptr_t name_to_utf8_address = 0x433FAB07u;      // __thiscall(&object->Name, char[128], 64)

        constexpr std::array<std::uint8_t, 6> frame_prologue{ 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8 };
        constexpr std::array<std::uint8_t, 7> serializer_prologue{ 0x53, 0x55, 0x57, 0x8B, 0xDA, 0x8B, 0xE9 };

        constexpr std::uint32_t Fnv1a(const char* text) noexcept
        {
            std::uint32_t hash = 0x811C9DC5u;
            for (; *text != '\0'; ++text) hash = (hash ^ static_cast<std::uint8_t>(*text)) * 0x01000193u;
            return hash;
        }

        constexpr std::uint32_t config_signature = 0xDEC00D60u;
        constexpr std::size_t config_header_size = 12;
        constexpr std::size_t unit_header_size = 8;
        constexpr std::uint16_t unit_type_array = 6; // LARRAY
        constexpr std::uint32_t scripts_unit_hash = Fnv1a("skeetsdk.config.scripts");
        constexpr std::uint8_t scripts_format = 1;
        constexpr std::size_t max_unit_payload = 0xFFFF;

        // Config tab: scripts are 0x20-byte entries {u64 time, int on_startup, std::wstring-like
        // name vector at +0xC} in the tab's chunk vector; the listbox shows them, and an item's
        // index (low 31 bits = script index) has its top bit set while the script is loaded.
        constexpr std::size_t tab_scripts_begin = 0x98;
        constexpr std::size_t tab_scripts_end = 0x9C;
        constexpr std::size_t tab_listbox = 0xC0;
        constexpr std::size_t script_entry_size = 0x20;
        constexpr std::size_t script_on_startup = 0x08;
        constexpr std::size_t script_name = 0x0C;
        constexpr std::size_t listbox_items = 0x90;
        constexpr std::size_t listbox_item_size = 0x14;
        constexpr std::uint32_t listbox_active_flag = 0x80000000u;

        // Scripts marked "load on startup" are meant to be always on, so a config that does not
        // list them does not unload them.
        constexpr bool keep_startup_scripts = true;

        // After scripts were loaded or unloaded, apply the config once more (Config_Load twice).
        // Off: with the scripts loaded first, one pass already sets every control (checked with the
        // diagnostics: the second pass changed nothing). Costs a second round of
        // pre/post_config_load events when on.
        constexpr bool reapply_after_script_changes = false;

        // Prints what the Load button does to the script controls to the console, step by step:
        // every control that differs from the saved config, shared names, and a re-check 1.5 s and 6 s
        // later. Very chatty with big scripts, so off.
        constexpr bool diagnostics = false;

        struct Vector // MSVC std::vector layout, allocated by the game's own allocator
        {
            std::uint8_t* first;
            std::uint8_t* last;
            std::uint8_t* end;
        };

        using ButtonFn = unsigned(__thiscall*)(void*, int);
        using SerializeFn = int(__fastcall*)(void*, void*);
        using LoadConfigFn = void(__fastcall*)(void*, void*, std::size_t);
        using LoadLuaFn = bool(__thiscall*)(const wchar_t*);
        using IsLuaLoadedFn = bool(__thiscall*)(const void*);
        using UpdateLuaInfoFn = void(__thiscall*)(void*);
        using LuaSlotFn = void**(__cdecl*)();
        using ResizeVectorFn = void(__thiscall*)(Vector*, std::size_t);
        using FindUnitFn = const void*(__fastcall*)(const void*, std::uint32_t, const void*);
        using NameToUtf8Fn = int(__thiscall*)(const void*, char*, int);

        struct InlineHook
        {
            std::uintptr_t target{};
            std::array<std::uint8_t, 16> original_bytes{};
            std::size_t patch_size{};
            std::uint8_t* trampoline{};
        };

        struct HookState
        {
            std::mutex mutex;
            bool installed{};
            InlineHook save_hook{};
            InlineHook load_hook{};
            InlineHook serialize_hook{};
            InlineHook load_config_hook{};
            ButtonFn original_save{};
            ButtonFn original_load{};
            SerializeFn original_serialize{};
            LoadConfigFn original_load_config{};
            LoadLuaFn load_lua{};
            LoadLuaFn unload_lua{};
            IsLuaLoadedFn is_lua_loaded{};
            UpdateLuaInfoFn update_lua_info{};
            LuaSlotFn lua_slot{};
            ResizeVectorFn resize_vector{};
        };

        HookState hook_state;

        // The menu, and with it every button handler, runs on a single thread.
        struct Session
        {
            void* saving_tab{};
            void* loading_tab{};
        };

        Session session;

        struct ScopedSlot
        {
            void*& slot;
            void* previous;
            ScopedSlot(void*& target, void* value) noexcept : slot(target), previous(target) { slot = value; }
            ~ScopedSlot() { slot = previous; }
            ScopedSlot(const ScopedSlot&) = delete;
            ScopedSlot& operator=(const ScopedSlot&) = delete;
        };

        template <typename T>
        T& At(void* base, std::size_t offset) noexcept
        {
            return *reinterpret_cast<T*>(static_cast<std::uint8_t*>(base) + offset);
        }

        bool IsReadable(const void* address, std::size_t size) noexcept
        {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(address, &info, sizeof(info)) == 0) return false;
            if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
            const auto end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
            return reinterpret_cast<std::uintptr_t>(address) + size <= end;
        }

        bool HasBytes(std::uintptr_t address, const std::uint8_t* expected, std::size_t size) noexcept
        {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(address);
            return IsReadable(bytes, size) && std::memcmp(bytes, expected, size) == 0;
        }

        // --- scripts of the config tab ---------------------------------------------------------

        struct ScriptTable
        {
            std::uint8_t* begin{};
            std::size_t count{};
            std::uint8_t* Entry(std::size_t index) const noexcept { return begin + index * script_entry_size; }
        };

        ScriptTable Scripts(void* tab) noexcept
        {
            auto* first = At<std::uint8_t*>(tab, tab_scripts_begin);
            auto* last = At<std::uint8_t*>(tab, tab_scripts_end);
            if (first == nullptr || last < first) return {};
            return { first, static_cast<std::size_t>(last - first) / script_entry_size };
        }

        std::wstring NameOf(std::uint8_t* entry)
        {
            const auto* first = At<const wchar_t*>(entry, script_name);
            const auto* last = At<const wchar_t*>(entry, script_name + sizeof(void*));
            if (first == nullptr || last <= first) return {};
            std::size_t length = static_cast<std::size_t>(last - first);
            while (length > 0 && first[length - 1] == L'\0') --length; // vector may include the terminator
            return std::wstring(first, length);
        }

        bool IsLoaded(std::uint8_t* entry) noexcept
        {
            return hook_state.is_lua_loaded(entry + script_name);
        }

        std::uint32_t* FindListItem(void* tab, std::size_t script_index) noexcept
        {
            auto* listbox = At<std::uint8_t*>(tab, tab_listbox);
            if (listbox == nullptr) return nullptr;
            auto* first = At<std::uint8_t*>(listbox, listbox_items);
            auto* last = At<std::uint8_t*>(listbox, listbox_items + sizeof(void*));
            if (first == nullptr || last < first) return nullptr;
            for (auto* item = first; item + listbox_item_size <= last; item += listbox_item_size)
            {
                auto* index = reinterpret_cast<std::uint32_t*>(item);
                if ((*index & ~listbox_active_flag) == script_index) return index;
            }
            return nullptr;
        }

        // Same steps as the Load script button handler.
        void LoadScript(void* tab, std::size_t index, std::uint8_t* entry) noexcept
        {
            const auto* name = At<const wchar_t*>(entry, script_name);
            if (name == nullptr || *name == L'\0' || !hook_state.load_lua(name)) return;
            void** slot = hook_state.lua_slot();
            if (slot == nullptr || *slot == nullptr || !IsLoaded(entry)) return;
            if (std::uint32_t* item = FindListItem(tab, index)) *item |= listbox_active_flag;
        }

        // Same steps as the Unload script button handler.
        void UnloadScript(void* tab, std::size_t index, std::uint8_t* entry) noexcept
        {
            const auto* name = At<const wchar_t*>(entry, script_name);
            if (name == nullptr || *name == L'\0') return;
            hook_state.unload_lua(name);
            if (std::uint32_t* item = FindListItem(tab, index)) *item &= ~listbox_active_flag;
        }

        bool Contains(const std::vector<std::wstring>& names, const std::wstring& name)
        {
            for (const std::wstring& candidate : names)
                if (_wcsicmp(candidate.c_str(), name.c_str()) == 0) return true;
            return false;
        }

        // Returns whether any script was loaded or unloaded.
        bool ApplyScripts(void* tab, const std::vector<std::wstring>& wanted)
        {
            bool changed = false;
            // The tables are fetched again every step: loading or unloading must never be able to
            // leave us holding a stale pointer.
            for (std::size_t index = 0;; ++index)
            {
                const ScriptTable table = Scripts(tab);
                if (index >= table.count) break;
                std::uint8_t* entry = table.Entry(index);
                if (!IsLoaded(entry)) continue;
                if (keep_startup_scripts && At<int>(entry, script_on_startup) != 0) continue;
                if (Contains(wanted, NameOf(entry))) continue;
                UnloadScript(tab, index, entry);
                changed = true;
            }

            for (const std::wstring& name : wanted)
            {
                const ScriptTable table = Scripts(tab);
                for (std::size_t index = 0; index < table.count; ++index)
                {
                    std::uint8_t* entry = table.Entry(index);
                    if (_wcsicmp(NameOf(entry).c_str(), name.c_str()) != 0) continue;
                    if (!IsLoaded(entry))
                    {
                        LoadScript(tab, index, entry);
                        changed = true;
                    }
                    break;
                }
            }

            hook_state.update_lua_info(tab);
            return changed;
        }

        // --- the unit ----------------------------------------------------------------------------
        // payload: u8 format, u16 count, count * { u16 length, utf-8 name }

        bool BuildPayload(void* tab, std::vector<std::uint8_t>& payload)
        {
            payload.assign({ scripts_format, 0, 0 });
            std::size_t count = 0;
            const ScriptTable table = Scripts(tab);
            for (std::size_t index = 0; index < table.count; ++index)
            {
                std::uint8_t* entry = table.Entry(index);
                const std::wstring name = NameOf(entry);
                if (name.empty() || !IsLoaded(entry)) continue;
                const int length = WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()),
                    nullptr, 0, nullptr, nullptr);
                if (length <= 0 || length > 0xFFFF) return false;
                const std::size_t at = payload.size();
                payload.resize(at + 2 + static_cast<std::size_t>(length));
                if (payload.size() > max_unit_payload) return false;
                payload[at] = static_cast<std::uint8_t>(length & 0xFF);
                payload[at + 1] = static_cast<std::uint8_t>(length >> 8);
                WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()),
                    reinterpret_cast<char*>(payload.data() + at + 2), length, nullptr, nullptr);
                ++count;
            }
            payload[1] = static_cast<std::uint8_t>(count & 0xFF);
            payload[2] = static_cast<std::uint8_t>(count >> 8);
            return true;
        }

        std::optional<std::vector<std::wstring>> DecodePayload(const std::uint8_t* data, std::size_t size)
        {
            if (size < 3 || data[0] != scripts_format) return std::nullopt;
            const std::size_t count = static_cast<std::size_t>(data[1]) | (static_cast<std::size_t>(data[2]) << 8);
            std::size_t offset = 3;
            std::vector<std::wstring> names;
            names.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
            {
                if (size - offset < 2) return std::nullopt;
                const std::size_t length = static_cast<std::size_t>(data[offset]) | (static_cast<std::size_t>(data[offset + 1]) << 8);
                offset += 2;
                if (size - offset < length) return std::nullopt;
                const int wide = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(data + offset),
                    static_cast<int>(length), nullptr, 0);
                std::wstring name(static_cast<std::size_t>(wide > 0 ? wide : 0), L'\0');
                if (wide > 0)
                    MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(data + offset),
                        static_cast<int>(length), name.data(), wide);
                names.push_back(std::move(name));
                offset += length;
            }
            if (offset != size) return std::nullopt;
            return names;
        }

        // Walks the units like the game does (size + 8 per unit) but does not stop at type 8.
        std::optional<std::vector<std::wstring>> FindScriptsUnit(const std::uint8_t* blob, std::size_t size)
        {
            if (blob == nullptr || size < config_header_size) return std::nullopt;
            std::uint32_t signature{};
            std::memcpy(&signature, blob, sizeof(signature));
            if (signature != config_signature) return std::nullopt;
            const std::uint8_t* cursor = blob + config_header_size;
            const std::uint8_t* const end = blob + size;
            while (static_cast<std::size_t>(end - cursor) >= unit_header_size)
            {
                std::uint16_t unit_size{};
                std::uint16_t unit_type{};
                std::uint32_t unit_hash{};
                std::memcpy(&unit_size, cursor, sizeof(unit_size));
                std::memcpy(&unit_type, cursor + 2, sizeof(unit_type));
                std::memcpy(&unit_hash, cursor + 4, sizeof(unit_hash));
                const std::uint8_t* data = cursor + unit_header_size;
                if (static_cast<std::size_t>(end - data) < unit_size) break;
                if (unit_hash == scripts_unit_hash && unit_type == unit_type_array)
                    return DecodePayload(data, unit_size);
                cursor = data + unit_size;
            }
            return std::nullopt;
        }

        void AppendScriptsUnit(Vector* blob, void* tab)
        {
            if (blob == nullptr || blob->first == nullptr || blob->last < blob->first) return;
            const std::size_t old_size = static_cast<std::size_t>(blob->last - blob->first);
            std::uint32_t signature{};
            if (old_size < config_header_size) return;
            std::memcpy(&signature, blob->first, sizeof(signature));
            if (signature != config_signature) return;

            std::vector<std::uint8_t> payload;
            if (!BuildPayload(tab, payload)) return; // too long to fit a unit: save without the list

            hook_state.resize_vector(blob, old_size + unit_header_size + payload.size());
            std::uint8_t* out = blob->first + old_size; // the vector may have moved
            const std::uint16_t unit_size = static_cast<std::uint16_t>(payload.size());
            std::memcpy(out, &unit_size, sizeof(unit_size));
            std::memcpy(out + 2, &unit_type_array, sizeof(unit_type_array));
            std::memcpy(out + 4, &scripts_unit_hash, sizeof(scripts_unit_hash));
            std::memcpy(out + unit_header_size, payload.data(), payload.size());
        }

        // --- diagnostics ---------------------------------------------------------------------------

        struct ControlInfo
        {
            std::uint32_t hash{};
            unsigned type{};
            unsigned size{};
            std::uint32_t value{};
            std::uint32_t saved{};
            bool comparable{};
            bool has_unit{};
            wchar_t name[56]{};
        };

        // Name of a control, read from its widget (the game decrypts names to UTF-16). Only for the
        // thread that owns the menu.
        void ControlName(const std::uint8_t* record, wchar_t (&out)[56]) noexcept
        {
            out[0] = L'\0';
            const std::uint8_t* widget = *reinterpret_cast<const std::uint8_t* const*>(record);
            if (widget == nullptr || !IsReadable(widget, 0x40)) return;
            wchar_t buffer[64]{};
            reinterpret_cast<NameToUtf8Fn>(name_to_utf8_address)(widget + 0x38, reinterpret_cast<char*>(buffer), 64);
            buffer[63] = L'\0';
            std::wcsncpy(out, buffer, 55);
            out[55] = L'\0';
        }

        // Every live script control with its current value and what `blob` has saved for it.
        std::vector<ControlInfo> CollectControls(const std::uint8_t* blob, std::size_t size, bool with_names)
        {
            std::vector<ControlInfo> controls;
            const auto find_unit = reinterpret_cast<FindUnitFn>(find_unit_address);
            const auto* slots = reinterpret_cast<const std::uint8_t* const*>(lua_records_address);
            if (!IsReadable(slots, 2 * sizeof(void*))) return controls;
            const std::uint8_t* begin = slots[0];
            const std::uint8_t* end = slots[1];
            if (begin == nullptr || end < begin) return controls;
            const std::size_t count = static_cast<std::size_t>(end - begin) / lua_record_size;
            for (std::size_t i = 0; i < count; ++i)
            {
                const std::uint8_t* record = begin + i * lua_record_size;
                if (!IsReadable(record, lua_record_size) || record[24] != 0) continue; // removed
                const std::uint8_t* node = *reinterpret_cast<const std::uint8_t* const*>(record + 4);
                if (node == nullptr || !IsReadable(node, 0x30)) continue;

                ControlInfo control;
                std::uint16_t node_size{};
                std::memcpy(&control.hash, node + 8, sizeof(control.hash));
                std::memcpy(&node_size, node + 12, sizeof(node_size));
                control.size = node_size;
                control.type = node[14];
                const unsigned offset = control.type < 13
                    ? *reinterpret_cast<const std::uint8_t*>(value_offset_table_address + control.type) : 0xFFu;
                if (offset != 0xFFu) std::memcpy(&control.value, node + offset, sizeof(control.value));
                control.comparable = offset != 0xFFu && control.type != 12;

                const std::uint8_t* unit = static_cast<const std::uint8_t*>(
                    find_unit(blob, static_cast<std::uint32_t>(size), node));
                if (unit != nullptr)
                {
                    control.has_unit = true;
                    std::uint16_t unit_size{};
                    std::memcpy(&unit_size, unit, sizeof(unit_size));
                    std::memcpy(&control.saved, unit + unit_header_size, std::min<std::size_t>(unit_size, sizeof(control.saved)));
                }
                if (with_names) ControlName(record, control.name);
                controls.push_back(control);
            }
            return controls;
        }

        // Summary plus the controls whose value is not what the blob saved, the ones the blob has
        // nothing for, and the ones that share a name (the game identifies a control by the hash of its
        // name and type only, so two scripts with a control of the same name read the same saved value).
        void ReportScriptValues(const char* when, const std::uint8_t* blob, std::size_t size, bool with_names) noexcept
        {
            if (!diagnostics) return;
            try
            {
                const std::vector<ControlInfo> controls = CollectControls(blob, size, with_names);
                unsigned with_unit = 0, no_unit = 0, differ = 0;
                const auto differs = [](const ControlInfo& c) {
                    const unsigned bytes = std::min(c.size, 4u);
                    const std::uint32_t mask = bytes >= 4 ? 0xFFFFFFFFu : ((1u << (8 * bytes)) - 1u);
                    return c.comparable && c.has_unit && (c.value & mask) != (c.saved & mask);
                };
                for (const ControlInfo& c : controls)
                {
                    if (c.has_unit) ++with_unit; else ++no_unit;
                    if (differs(c)) ++differ;
                }
                std::vector<char> seen(controls.size(), 0);
                unsigned duplicate_groups = 0;
                for (std::size_t i = 0; i < controls.size(); ++i)
                {
                    if (seen[i]) continue;
                    for (std::size_t j = i + 1; j < controls.size(); ++j)
                        if (controls[j].hash == controls[i].hash && controls[j].type == controls[i].type)
                            seen[i] = seen[j] = 1;
                    if (seen[i]) ++duplicate_groups;
                }
                std::printf("[config scripts] %s: %u controls, %u with saved value, %u without, %u differ from saved, %u names shared\n",
                    when, static_cast<unsigned>(controls.size()), with_unit, no_unit, differ, duplicate_groups);

                unsigned shown = 0;
                for (const ControlInfo& c : controls)
                {
                    if (shown >= 60) break;
                    if (differs(c))
                    {
                        std::printf("  DIFFERS \"%ls\" hash=%08X type=%u size=%u value=%08X saved=%08X\n", c.name,
                            static_cast<unsigned>(c.hash), c.type, c.size, static_cast<unsigned>(c.value),
                            static_cast<unsigned>(c.saved));
                        ++shown;
                    }
                }
                shown = 0;
                for (const ControlInfo& c : controls)
                {
                    if (shown >= 60) break;
                    if (!c.has_unit)
                    {
                        std::printf("  NOT SAVED \"%ls\" hash=%08X type=%u size=%u value=%08X\n", c.name,
                            static_cast<unsigned>(c.hash), c.type, c.size, static_cast<unsigned>(c.value));
                        ++shown;
                    }
                }
                std::vector<char> printed(controls.size(), 0);
                shown = 0;
                for (std::size_t i = 0; i < controls.size() && shown < 40; ++i)
                {
                    if (printed[i]) continue;
                    unsigned members = 1;
                    for (std::size_t j = i + 1; j < controls.size(); ++j)
                        if (controls[j].hash == controls[i].hash && controls[j].type == controls[i].type) ++members;
                    if (members < 2) continue;
                    std::printf("  SHARED hash=%08X type=%u x%u:", static_cast<unsigned>(controls[i].hash), controls[i].type, members);
                    for (std::size_t j = i; j < controls.size(); ++j)
                    {
                        if (controls[j].hash != controls[i].hash || controls[j].type != controls[i].type) continue;
                        printed[j] = 1;
                        std::printf(" \"%ls\"(%08X)", controls[j].name, static_cast<unsigned>(controls[j].value));
                    }
                    std::printf("\n");
                    ++shown;
                }
            }
            catch (...) {}
        }

        // Looks again a while after the Load button returned, from another thread, on a copy of the
        // blob. Reads only. Shows whether something changed the values after the config was applied.
        struct DelayedCheck
        {
            std::vector<std::uint8_t> blob;
            unsigned delay_ms;
            const char* label;
        };

        DWORD WINAPI DelayedCheckThread(LPVOID parameter) noexcept
        {
            std::unique_ptr<DelayedCheck> check(static_cast<DelayedCheck*>(parameter));
            Sleep(check->delay_ms);
            ReportScriptValues(check->label, check->blob.data(), check->blob.size(), false);
            return 0;
        }

        void ScheduleDelayedCheck(const std::uint8_t* blob, std::size_t size, unsigned delay_ms, const char* label) noexcept
        {
            if (!diagnostics) return;
            try
            {
                auto check = std::make_unique<DelayedCheck>(DelayedCheck{ std::vector<std::uint8_t>(blob, blob + size), delay_ms, label });
                if (HANDLE thread = CreateThread(nullptr, 0, DelayedCheckThread, check.get(), 0, nullptr))
                {
                    check.release();
                    CloseHandle(thread);
                }
            }
            catch (...) {}
        }

        // --- hooks -------------------------------------------------------------------------------

        unsigned __fastcall SaveHandlerHook(void* tab, void*, int argument) noexcept
        {
            ScopedSlot scope(session.saving_tab, tab);
            return hook_state.original_save(tab, argument);
        }

        int __fastcall SerializeHook(void* defaults, void* output) noexcept
        {
            const int result = hook_state.original_serialize(defaults, output);
            if (session.saving_tab != nullptr)
            {
                try { AppendScriptsUnit(static_cast<Vector*>(output), session.saving_tab); }
                catch (...) {}
            }
            return result;
        }

        void __fastcall LoadConfigHook(void* menu, void* data, std::size_t size) noexcept
        {
            if (void* tab = session.loading_tab)
            {
                // Scripts first, values second: the original only applies values to controls that
                // exist right now. Cleared while the scripts run so a script that calls
                // config.load() cannot start another round.
                ScopedSlot scope(session.loading_tab, nullptr);
                bool scripts_changed = false;
                const auto* bytes = static_cast<const std::uint8_t*>(data);
                try
                {
                    auto wanted = FindScriptsUnit(bytes, size);
                    if (diagnostics)
                    {
                        std::printf("[config scripts] Load: blob %u bytes, ", static_cast<unsigned>(size));
                        if (wanted)
                        {
                            std::printf("script list:");
                            for (const std::wstring& name : *wanted) std::printf(" [%ls]", name.c_str());
                            std::printf("\n");
                        }
                        else std::printf("no script list (scripts left alone)\n");
                        ReportScriptValues("before loading scripts", bytes, size, true);
                    }
                    if (wanted) scripts_changed = ApplyScripts(tab, *wanted);
                }
                catch (...) {}
                if (diagnostics)
                    std::printf("[config scripts] scripts changed: %s\n", scripts_changed ? "yes" : "no");
                ReportScriptValues("scripts loaded, before applying the config", bytes, size, true);
                hook_state.original_load_config(menu, data, size);
                ReportScriptValues("after applying the config (pass 1)", bytes, size, true);
                // Loading a script rebuilds the whole Lua state; applying the config a second time
                // is what pressing Load twice does by hand, so values of freshly loaded scripts
                // cannot be missed whatever the rebuild leaves behind.
                if (scripts_changed && reapply_after_script_changes)
                {
                    hook_state.original_load_config(menu, data, size);
                    ReportScriptValues("after applying the config (pass 2)", bytes, size, true);
                }
                ScheduleDelayedCheck(bytes, size, 1500, "1.5 s after Load");
                ScheduleDelayedCheck(bytes, size, 6000, "6 s after Load");
                return;
            }
            hook_state.original_load_config(menu, data, size);
        }

        unsigned __fastcall LoadHandlerHook(void* tab, void*, int argument) noexcept
        {
            ScopedSlot scope(session.loading_tab, tab);
            return hook_state.original_load(tab, argument);
        }

        // --- installation --------------------------------------------------------------------------

        void WriteJump(std::uint8_t* source, const void* destination) noexcept
        {
            const auto source_after_jump = reinterpret_cast<std::uintptr_t>(source + 5);
            const auto target = reinterpret_cast<std::uintptr_t>(destination);
            source[0] = 0xE9;
            *reinterpret_cast<std::uint32_t*>(source + 1) = static_cast<std::uint32_t>(target - source_after_jump);
        }

        // The copied prologue must hold whole position-independent instructions only.
        template <typename Fn, std::size_t N>
        bool InstallHook(InlineHook& hook, std::uintptr_t address, const std::array<std::uint8_t, N>& prologue,
            const void* replacement, Fn& original) noexcept
        {
            static_assert(N >= 5 && N <= 16);
            if (!HasBytes(address, prologue.data(), N)) return false;
            auto* target = reinterpret_cast<std::uint8_t*>(address);
            auto* trampoline = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, N + 5,
                MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
            if (trampoline == nullptr) return false;
            std::memcpy(hook.original_bytes.data(), target, N);
            std::memcpy(trampoline, target, N);
            WriteJump(trampoline + N, target + N);
            DWORD old_protection{};
            if (!VirtualProtect(target, N, PAGE_EXECUTE_READWRITE, &old_protection))
            {
                VirtualFree(trampoline, 0, MEM_RELEASE);
                return false;
            }
            original = reinterpret_cast<Fn>(trampoline); // ready before the jump goes live
            WriteJump(target, replacement);
            if (N > 5) std::memset(target + 5, 0x90, N - 5);
            DWORD ignored{};
            VirtualProtect(target, N, old_protection, &ignored);
            FlushInstructionCache(GetCurrentProcess(), target, N);
            hook.target = address;
            hook.patch_size = N;
            hook.trampoline = trampoline;
            return true;
        }

        void RemoveHook(InlineHook& hook) noexcept
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
        }

        void RemoveAll() noexcept
        {
            RemoveHook(hook_state.save_hook);
            RemoveHook(hook_state.load_hook);
            RemoveHook(hook_state.serialize_hook);
            RemoveHook(hook_state.load_config_hook);
            hook_state.original_save = nullptr;
            hook_state.original_load = nullptr;
            hook_state.original_serialize = nullptr;
            hook_state.original_load_config = nullptr;
            hook_state.installed = false;
        }

        // Routines we only call: make sure they are what the IDB says before anything is patched.
        bool HelpersAreValid() noexcept
        {
            struct Helper { std::uintptr_t address; std::array<std::uint8_t, 6> bytes; };
            static constexpr Helper helpers[] = {
                { load_lua_address, { 0x83, 0xEC, 0x4C, 0x53, 0x55, 0x33 } },
                { unload_lua_address, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14 } },
                { is_lua_loaded_address, { 0x51, 0x8B, 0xD1, 0x56, 0x8B, 0x0A } },
                { update_lua_info_address, { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8 } },
                { lua_slot_address, { 0xA1, 0x00, 0x5B, 0x47, 0x43, 0x85 } },
                { resize_vector_address, { 0x8B, 0x41, 0x04, 0x8B, 0x11, 0x2B } },
                { find_unit_address, { 0x56, 0x57, 0x85, 0xC9, 0x74, 0x4F } },
                { name_to_utf8_address, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C } },
            };
            for (const Helper& helper : helpers)
                if (!HasBytes(helper.address, helper.bytes.data(), helper.bytes.size())) return false;
            return true;
        }
    }

    bool Install() noexcept
    {
        std::scoped_lock lock(hook_state.mutex);
        if (hook_state.installed) return true;
        if (!HelpersAreValid()) return false;

        hook_state.load_lua = reinterpret_cast<LoadLuaFn>(load_lua_address);
        hook_state.unload_lua = reinterpret_cast<LoadLuaFn>(unload_lua_address);
        hook_state.is_lua_loaded = reinterpret_cast<IsLuaLoadedFn>(is_lua_loaded_address);
        hook_state.update_lua_info = reinterpret_cast<UpdateLuaInfoFn>(update_lua_info_address);
        hook_state.lua_slot = reinterpret_cast<LuaSlotFn>(lua_slot_address);
        hook_state.resize_vector = reinterpret_cast<ResizeVectorFn>(resize_vector_address);

        if (!InstallHook(hook_state.save_hook, save_handler_address, frame_prologue,
                reinterpret_cast<const void*>(&SaveHandlerHook), hook_state.original_save)
            || !InstallHook(hook_state.serialize_hook, serializer_address, serializer_prologue,
                reinterpret_cast<const void*>(&SerializeHook), hook_state.original_serialize)
            || !InstallHook(hook_state.load_config_hook, load_config_address, frame_prologue,
                reinterpret_cast<const void*>(&LoadConfigHook), hook_state.original_load_config)
            || !InstallHook(hook_state.load_hook, load_handler_address, frame_prologue,
                reinterpret_cast<const void*>(&LoadHandlerHook), hook_state.original_load))
        {
            RemoveAll();
            return false;
        }

        hook_state.installed = true;
        return true;
    }

    void Remove() noexcept
    {
        std::scoped_lock lock(hook_state.mutex);
        if (hook_state.installed) RemoveAll();
    }

    bool IsInstalled() noexcept
    {
        std::scoped_lock lock(hook_state.mutex);
        return hook_state.installed;
    }
#else
    bool Install() noexcept { return false; }
    void Remove() noexcept {}
    bool IsInstalled() noexcept { return false; }
#endif
}
