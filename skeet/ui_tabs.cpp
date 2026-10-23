#include "ui_tabs.hpp"

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
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Tabs and containers are made with the same routines the menu builder uses for the LUA tab
// (TabCon -> InsertTab -> ChildCon -> InsertChild -> hide). The game's own ui.new_* finds a tab and
// a container by comparing the string it is given with each tab's / child's name, so tabs made here
// work with it without any change to the Lua API itself.
//
// Every script load / unload / reload closes the whole Lua state and runs the remaining scripts in a
// new one (the game removes the script's controls itself). A hook on the state teardown removes the
// tabs made by new_tab at the same moment, and the scripts that are still loaded make theirs again,
// so a tab lives exactly as long as the script that made it.

namespace skeetsdk::ui_tabs
{
#if defined(_M_IX86)
    namespace
    {
        // Lua C API in the mapped image (same addresses the Lua guard uses).
        constexpr std::uintptr_t lua_push_cclosure_address = 0x43432F73u;
        constexpr std::uintptr_t lua_set_field_address = 0x43435B37u;
        constexpr std::uintptr_t lua_push_string_address = 0x434370E4u;
        constexpr std::uintptr_t lua_set_top_address = 0x43426BDFu;
        constexpr std::uintptr_t lua_find_table_address = 0x43426632u;
        constexpr std::uintptr_t lua_push_value_address = 0x4342CEA7u;
        constexpr std::uintptr_t lua_type_address = 0x4343654Au;
        constexpr std::uintptr_t lua_to_lstring_address = 0x43435597u;
        constexpr std::uintptr_t lua_get_field_address = 0x4342EBA2u;
        constexpr int lua_registry_index = -10000;
        constexpr int lua_type_number = 3;
        constexpr int lua_type_string = 4;
        constexpr int lua_type_function = 6;
        constexpr std::int32_t itype_false = -2;
        constexpr std::int32_t itype_true = -3;

        // Menu code (verified in the IDB; see ConfigTab_*/menu builder at 0x4348D300).
        constexpr std::uintptr_t menu_pointer_address = 0x43476924u;   // holds the CMenu*
        constexpr std::uintptr_t alloc_address = 0x43387448u;          // __thiscall(size in ecx) -> ptr
        constexpr std::uintptr_t tab_constructor_address = 0x4348E41Du;
        constexpr std::uintptr_t insert_tab_address = 0x43342275u;
        constexpr std::uintptr_t child_constructor_address = 0x4348210Cu;
        constexpr std::uintptr_t insert_child_address = 0x4347DE9Bu;
        constexpr std::uintptr_t set_visible_address = 0x433E000Bu;
        constexpr std::uintptr_t name_to_wide_address = 0x433FAB07u;   // decrypts an object's name into wchar_t[]
        constexpr std::uintptr_t tab_switch_address = 0x433B75D3u;     // __thiscall(menu, tab index)
        constexpr std::uintptr_t lua_teardown_address = 0x4331FDC3u;   // closes the Lua state, takes no arguments
        constexpr std::size_t menu_current_tab = 0x64;
        constexpr std::size_t tab_index_offset = 0x0C;
        constexpr unsigned lua_tab_index = 8;
        constexpr int tab_icon_step = 64;                              // vertical distance between tab icons

        // push ebx/ebp/esi/edi after sub esp, 2Ch: whole instructions, nothing position dependent.
        constexpr std::array<std::uint8_t, 7> lua_teardown_prologue{ 0x83, 0xEC, 0x2C, 0x53, 0x55, 0x56, 0x57 };
        constexpr std::array<std::uint8_t, 6> tab_switch_prologue{ 0x56, 0x8B, 0xF1, 0x57, 0x8B, 0x7C };

        constexpr std::size_t tab_object_size = 0x98;
        constexpr std::size_t child_object_size = 0xC8;
        constexpr std::size_t vtable_reset_layout = 14;                // ITab::ResetLayout

        constexpr std::size_t menu_tabs_begin = 0x54;
        constexpr std::size_t menu_tabs_end = 0x58;
        constexpr std::size_t object_name = 0x38;                      // shared_ptr<XorW> of tabs and children
        constexpr std::size_t tab_pos = 0x20;
        constexpr std::size_t tab_childs_begin = 0x70;
        constexpr std::size_t tab_childs_end = 0x74;
        // TabIcon, read by the tab draw routine: the texture id (-1 draws the tab's name as text, like
        // the built-in LUA tab), then the source rectangle {x, y, width, height} of that texture, drawn
        // centered in the tab's 75 x 64 cell. A zero size draws nothing.
        constexpr std::size_t tab_icon_texture_id = 0x80;
        constexpr std::size_t tab_icon_source = 0x84;
        constexpr int tab_icon_pixels = 48;                            // size of the built-in icons
        constexpr std::size_t name_buffer_size = 128;                  // bytes, what name_to_wide wants
        constexpr std::size_t max_name_bytes = 63;
        constexpr int tab_blocks = 24;

        // A, B columns of the LUA tab: {x, width, y, height} in blocks, packed little endian.
        constexpr std::uint32_t Layout(int x, int width, int y, int height) noexcept
        {
            return static_cast<std::uint32_t>(x) | (static_cast<std::uint32_t>(width) << 8)
                | (static_cast<std::uint32_t>(y) << 16) | (static_cast<std::uint32_t>(height) << 24);
        }

        struct TValue
        {
            std::uint32_t value;
            std::int32_t type;
        };

        using LuaCFunction = int(__cdecl*)(void*);
        using PushCClosureFn = void(__cdecl*)(void*, LuaCFunction, int);
        using SetFieldFn = void(__cdecl*)(void*, int, const char*);
        using PushStringFn = void(__cdecl*)(void*, const char*);
        using SetTopFn = void(__cdecl*)(void*, int);
        using FindTableFn = const char*(__cdecl*)(void*, int, const char*, int);
        using PushValueFn = void(__cdecl*)(void*, int);
        using TypeFn = int(__cdecl*)(void*, int);
        using ToLStringFn = const char*(__cdecl*)(void*, int, std::size_t*);
        using GetFieldFn = void(__cdecl*)(void*, int, const char*);

        using AllocFn = void*(__fastcall*)(std::size_t);
        using TabConFn = void*(__thiscall*)(void*, void*, const wchar_t*, int*);
        using InsertTabFn = void*(__thiscall*)(void*, void*);
        using ChildConFn = void*(__thiscall*)(void*, void*, const wchar_t*, std::uint32_t, std::uint32_t, std::uint32_t);
        using InsertChildFn = void*(__thiscall*)(void*, void*);
        using SetVisibleFn = void(__thiscall*)(void*, char);
        using NameToWideFn = int(__thiscall*)(const void*, char*, int);
        using VirtualFn = void(__thiscall*)(void*);
        using TabSwitchFn = void(__thiscall*)(void*, unsigned);
        using TeardownFn = int(__cdecl*)();

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
            InlineHook teardown_hook{};
            TeardownFn original_teardown{};
        };

        HookState hook_state;

        // Tabs made through new_tab that are still in the menu.
        std::vector<void*> script_tabs;

        const auto push_cclosure = reinterpret_cast<PushCClosureFn>(lua_push_cclosure_address);
        const auto set_field = reinterpret_cast<SetFieldFn>(lua_set_field_address);
        const auto push_string = reinterpret_cast<PushStringFn>(lua_push_string_address);
        const auto set_top = reinterpret_cast<SetTopFn>(lua_set_top_address);
        const auto find_table = reinterpret_cast<FindTableFn>(lua_find_table_address);
        const auto push_value = reinterpret_cast<PushValueFn>(lua_push_value_address);
        const auto get_type = reinterpret_cast<TypeFn>(lua_type_address);
        const auto to_lstring = reinterpret_cast<ToLStringFn>(lua_to_lstring_address);
        const auto get_field = reinterpret_cast<GetFieldFn>(lua_get_field_address);

        const auto alloc = reinterpret_cast<AllocFn>(alloc_address);
        const auto tab_constructor = reinterpret_cast<TabConFn>(tab_constructor_address);
        const auto insert_tab = reinterpret_cast<InsertTabFn>(insert_tab_address);
        const auto child_constructor = reinterpret_cast<ChildConFn>(child_constructor_address);
        const auto insert_child = reinterpret_cast<InsertChildFn>(insert_child_address);
        const auto set_visible = reinterpret_cast<SetVisibleFn>(set_visible_address);
        const auto name_to_wide = reinterpret_cast<NameToWideFn>(name_to_wide_address);
        const auto tab_switch = reinterpret_cast<TabSwitchFn>(tab_switch_address);

        template <typename T>
        T& At(void* base, std::size_t offset) noexcept
        {
            return *reinterpret_cast<T*>(static_cast<std::uint8_t*>(base) + offset);
        }

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

        // --- Lua arguments ------------------------------------------------------------------------

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

        bool ReadString(void* lua_state, int index, std::string& out)
        {
            if (get_type(lua_state, index) != lua_type_string) return false;
            std::size_t length = 0;
            const char* text = to_lstring(lua_state, index, &length);
            if (text == nullptr) return false;
            out.assign(text, length);
            return true;
        }

        // Optional number argument in [0, tab_blocks]: absent / nil keeps `value`.
        bool ReadBlocks(void* lua_state, int index, int& value) noexcept
        {
            const TValue* argument = Argument(lua_state, index);
            if (argument == nullptr || argument->type == -1 /* nil */) return true;
            if (get_type(lua_state, index) != lua_type_number) return false;
            double number = 0.0;
            std::memcpy(&number, argument, sizeof(number));
            if (!std::isfinite(number) || number < 0.0 || number > tab_blocks) return false;
            value = static_cast<int>(number);
            return true;
        }

        // Optional texture id (a whole number >= 0): absent / nil leaves `value` empty.
        bool ReadTextureId(void* lua_state, int index, std::optional<int>& value) noexcept
        {
            const TValue* argument = Argument(lua_state, index);
            if (argument == nullptr || argument->type == -1 /* nil */) return true;
            if (get_type(lua_state, index) != lua_type_number) return false;
            double number = 0.0;
            std::memcpy(&number, argument, sizeof(number));
            if (!std::isfinite(number) || number < 0.0 || number != std::floor(number)
                || number > static_cast<double>(std::numeric_limits<int>::max()))
                return false;
            value = static_cast<int>(number);
            return true;
        }

        // Optional place in the tab bar: a whole number >= 1, "first" or "last". Absent / nil / "last"
        // leaves `value` empty, which means the bottom.
        bool ReadTabPlace(void* lua_state, int index, std::optional<unsigned>& value) noexcept
        {
            const TValue* argument = Argument(lua_state, index);
            if (argument == nullptr || argument->type == -1 /* nil */) return true;
            const int type = get_type(lua_state, index);
            if (type == lua_type_string)
            {
                std::size_t length = 0;
                const char* text = to_lstring(lua_state, index, &length);
                if (text == nullptr) return false;
                if (length == 5 && std::memcmp(text, "first", 5) == 0) { value = 1; return true; }
                return length == 4 && std::memcmp(text, "last", 4) == 0;
            }
            if (type != lua_type_number) return false;
            double number = 0.0;
            std::memcpy(&number, argument, sizeof(number));
            if (!std::isfinite(number) || number < 1.0 || number != std::floor(number)
                || number > static_cast<double>(std::numeric_limits<int>::max()))
                return false;
            value = static_cast<unsigned>(number);
            return true;
        }

        // Pushes true/false straight onto the stack (lua_pushboolean is not needed elsewhere).
        // A C function is guaranteed LUA_MINSTACK free slots.
        void PushBoolean(void* lua_state, bool value) noexcept
        {
            auto** top = reinterpret_cast<TValue**>(static_cast<std::uint8_t*>(lua_state) + 0x14);
            (*top)->value = 0;
            (*top)->type = value ? itype_true : itype_false;
            ++*top;
        }

        int ReturnBoolean(void* lua_state, bool value) noexcept
        {
            PushBoolean(lua_state, value);
            return 1;
        }

        int Fail(void* lua_state, const char* message) noexcept
        {
            push_string(lua_state, nullptr);
            push_string(lua_state, message);
            return 2;
        }

        // --- menu ---------------------------------------------------------------------------------

        // The menu handed over by ReserveTabSlots (the SDK's pointer, known good where it is called),
        // else the global the game's own Lua API reads.
        void* known_menu = nullptr;

        void* Menu() noexcept
        {
            if (known_menu != nullptr) return known_menu;
            const auto* slot = reinterpret_cast<void* const*>(menu_pointer_address);
            return IsReadable(slot, sizeof(void*)) ? *slot : nullptr;
        }

        bool Utf8ToWide(const std::string& text, std::wstring& out)
        {
            out.clear();
            if (text.empty()) return false;
            const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                static_cast<int>(text.size()), nullptr, 0);
            if (length <= 0) return false;
            out.assign(static_cast<std::size_t>(length), L'\0');
            return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                out.data(), length) == length;
        }

        bool ValidName(const std::string& name) noexcept
        {
            return !name.empty() && name.size() <= max_name_bytes && name.find('\0') == std::string::npos;
        }

        // Names are stored encrypted and come out as UTF-16 (the game compares them with wcscmp).
        bool HasName(void* object, const std::wstring& name) noexcept
        {
            wchar_t buffer[name_buffer_size / sizeof(wchar_t)]{};
            name_to_wide(static_cast<std::uint8_t*>(object) + object_name, reinterpret_cast<char*>(buffer), 64);
            buffer[name_buffer_size / sizeof(wchar_t) - 1] = L'\0';
            return name == buffer;
        }

        // The menu's tabs / a tab's containers are vectors of pointers.
        template <typename Predicate>
        void* FindIn(void* owner, std::size_t begin_offset, std::size_t end_offset, Predicate matches) noexcept
        {
            auto* first = At<std::uint8_t*>(owner, begin_offset);
            auto* last = At<std::uint8_t*>(owner, end_offset);
            if (first == nullptr || last < first) return nullptr;
            for (auto* slot = first; slot + sizeof(void*) <= last; slot += sizeof(void*))
            {
                void* object = *reinterpret_cast<void**>(slot);
                if (object != nullptr && matches(object)) return object;
            }
            return nullptr;
        }

        void* FindTab(void* menu, const std::wstring& name) noexcept
        {
            return FindIn(menu, menu_tabs_begin, menu_tabs_end, [&](void* tab) { return HasName(tab, name); });
        }

        void* FindContainer(void* tab, const std::wstring& name) noexcept
        {
            return FindIn(tab, tab_childs_begin, tab_childs_end, [&](void* child) { return HasName(child, name); });
        }

        void ResetLayout(void* tab) noexcept
        {
            (*reinterpret_cast<VirtualFn**>(tab))[vtable_reset_layout](tab);
        }

        // The menu and the tabs keep their children in {first, last, end} vectors of pointers.
        // A vector must never reallocate while the menu walks it: a script is loaded by a click, and
        // that click is dispatched from inside a loop over Menu->Tabs (and over the clicked tab's
        // children) that holds a pointer into the buffer. Growing it there leaves the loop reading
        // freed memory. So room is reserved up front, and adding refuses when there is none.
        std::size_t SpareSlots(void* owner, std::size_t first_offset) noexcept
        {
            const auto* last = At<std::uint8_t*>(owner, first_offset + sizeof(void*));
            const auto* end = At<std::uint8_t*>(owner, first_offset + 2 * sizeof(void*));
            return last != nullptr && end >= last ? static_cast<std::size_t>(end - last) / sizeof(void*) : 0;
        }

        // Grows the vector's storage to `capacity` pointers. The old buffer is left allocated on
        // purpose (a few bytes) so anything still pointing into it stays readable.
        bool ReservePointers(void* owner, std::size_t first_offset, std::size_t capacity) noexcept
        {
            auto& first = At<std::uint8_t*>(owner, first_offset);
            auto& last = At<std::uint8_t*>(owner, first_offset + sizeof(void*));
            auto& end = At<std::uint8_t*>(owner, first_offset + 2 * sizeof(void*));
            if (first != nullptr && (last < first || end < last)) return false;
            const std::size_t size = first != nullptr ? static_cast<std::size_t>(last - first) / sizeof(void*) : 0;
            const std::size_t current = first != nullptr ? static_cast<std::size_t>(end - first) / sizeof(void*) : 0;
            if (current >= capacity) return true;
            auto* storage = static_cast<std::uint8_t*>(alloc(capacity * sizeof(void*)));
            if (storage == nullptr) return false;
            if (size != 0) std::memcpy(storage, first, size * sizeof(void*));
            first = storage;
            last = storage + size * sizeof(void*);
            end = storage + capacity * sizeof(void*);
            return true;
        }

        void* AddContainer(void* tab, const std::wstring& name, std::uint32_t layout) noexcept
        {
            void* child = alloc(child_object_size);
            if (child == nullptr) return nullptr;
            child_constructor(child, tab, name.c_str(), layout, 0, 1);
            insert_child(tab, child);
            return child;
        }

        constexpr std::size_t tab_slots_reserved = 16;
        constexpr std::size_t container_slots_reserved = 8;

        // --- the tab bar ----------------------------------------------------------------------------

        // The bar draws every tab's icon at the tab's own position, one step below the previous one.
        // The Menu->Tabs vector only decides tab numbers and navigation order, so putting a tab at the
        // top or in the middle of the bar means moving icon positions, never reordering the vector:
        // the game indexes that vector and walks it while a click is being handled, and shifting a
        // built-in tab's slot there is not safe.
        template <typename Visit>
        void ForEachTab(void* menu, Visit visit) noexcept
        {
            const auto* first = At<std::uint8_t*>(menu, menu_tabs_begin);
            const auto* last = At<std::uint8_t*>(menu, menu_tabs_end);
            if (first == nullptr || last < first) return;
            for (const auto* slot = first; slot + sizeof(void*) <= last; slot += sizeof(void*))
            {
                if (void* tab = *reinterpret_cast<void* const*>(slot)) visit(tab);
            }
        }

        int& IconY(void* tab) noexcept
        {
            return At<int>(tab, tab_pos + 4);
        }

        // Icon y for a new tab that goes `place` (1-based) from the top of the bar, or at the bottom when
        // `place` is empty or past the last tab. Tabs from that slot down move one step lower.
        int MakeRoomInTabBar(void* menu, std::optional<unsigned> place) noexcept
        {
            int bottom = std::numeric_limits<int>::min();
            std::size_t count = 0;
            ForEachTab(menu, [&](void* tab) { bottom = std::max(bottom, IconY(tab)); ++count; });
            if (!place || *place > count) return bottom + tab_icon_step;

            int slot = bottom; // the y with exactly place - 1 icons above it
            ForEachTab(menu, [&](void* tab)
            {
                std::size_t above = 0;
                ForEachTab(menu, [&](void* other) { if (IconY(other) < IconY(tab)) ++above; });
                if (above == *place - 1) slot = IconY(tab);
            });
            ForEachTab(menu, [&](void* tab) { if (IconY(tab) >= slot) IconY(tab) += tab_icon_step; });
            return slot;
        }

        // --- removing tabs --------------------------------------------------------------------------

        // The menu, unless the game already destroyed it (it clears the global when it does).
        void* LiveMenu() noexcept
        {
            const auto* slot = reinterpret_cast<void* const*>(menu_pointer_address);
            if (!IsReadable(slot, sizeof(void*)) || *slot == nullptr) return nullptr;
            return known_menu != nullptr ? known_menu : *slot;
        }

        // Takes the tab out of Menu->Tabs. The tab and its containers are leaked on purpose: nothing
        // may be left pointing at freed memory, and it is a few hundred bytes. Later tabs are
        // renumbered, and the tabs below it in the bar move up to close the gap.
        void RemoveTab(void* menu, void* tab) noexcept
        {
            auto& first = At<std::uint8_t*>(menu, menu_tabs_begin);
            auto& last = At<std::uint8_t*>(menu, menu_tabs_end);
            if (first == nullptr || last < first) return;
            std::uint8_t* slot = nullptr;
            for (auto* p = first; p + sizeof(void*) <= last; p += sizeof(void*))
            {
                if (*reinterpret_cast<void**>(p) == tab) { slot = p; break; }
            }
            if (slot == nullptr) return;

            const std::size_t index = static_cast<std::size_t>(slot - first) / sizeof(void*);
            auto& current = At<std::uint32_t>(menu, menu_current_tab);
            // Leave it while it is still in the list, so the game closes it properly.
            if (current == index) tab_switch(menu, lua_tab_index);

            const int removed_y = IconY(tab);
            std::memmove(slot, slot + sizeof(void*), static_cast<std::size_t>(last - (slot + sizeof(void*))));
            last -= sizeof(void*);
            if (current > index) --current;

            for (auto* p = slot; p + sizeof(void*) <= last; p += sizeof(void*))
                At<int>(*reinterpret_cast<void**>(p), tab_index_offset) = static_cast<int>((p - first) / sizeof(void*));
            ForEachTab(menu, [&](void* below) { if (IconY(below) > removed_y) IconY(below) -= tab_icon_step; });
        }

        void RemoveScriptTabs() noexcept
        {
            std::vector<void*> tabs;
            tabs.swap(script_tabs);
            void* menu = LiveMenu();
            if (menu == nullptr) return;
            for (auto it = tabs.rbegin(); it != tabs.rend(); ++it) RemoveTab(menu, *it);
        }

        // Runs after the game closed the Lua state and removed the script's controls.
        int __cdecl LuaTeardownHook() noexcept
        {
            const int result = hook_state.original_teardown();
            RemoveScriptTabs();
            return result;
        }

        // --- inline hook (same scheme as the Lua guard) ----------------------------------------------

        bool HasBytes(std::uintptr_t address, const std::uint8_t* expected, std::size_t size) noexcept
        {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(address);
            return IsReadable(bytes, size) && std::memcmp(bytes, expected, size) == 0;
        }

        void WriteJump(std::uint8_t* source, const void* destination) noexcept
        {
            const auto source_after_jump = reinterpret_cast<std::uintptr_t>(source + 5);
            const auto target = reinterpret_cast<std::uintptr_t>(destination);
            source[0] = 0xE9;
            *reinterpret_cast<std::uint32_t*>(source + 1) = static_cast<std::uint32_t>(target - source_after_jump);
        }

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

        enum class Status { Created, Exists, NoMenu, NoSlot, NoMemory };

        struct TabOptions
        {
            std::optional<int> icon;            // texture id, else the LUA tab's icon
            bool default_columns = true;        // columns "A" and "B"
            std::optional<unsigned> place;      // 1-based from the top of the tab bar, else last
        };

        Status CreateTab(const std::string& name, const std::wstring& wide_name, const TabOptions& options)
        {
            void* menu = Menu();
            if (menu == nullptr || At<std::uint8_t*>(menu, menu_tabs_begin) == At<std::uint8_t*>(menu, menu_tabs_end))
                return Status::NoMenu;
            if (FindTab(menu, wide_name) != nullptr) return Status::Exists;
            if (SpareSlots(menu, menu_tabs_begin) < 1) return Status::NoSlot; // see SpareSlots

            void* tab = alloc(tab_object_size);
            if (tab == nullptr) return Status::NoMemory;

            // The icon goes in the bar's column, either below the last icon (the builder's running
            // position) or in the slot asked for, with the icons below it moved down.
            auto* last_slot = At<std::uint8_t*>(menu, menu_tabs_end) - sizeof(void*);
            void* last_tab = *reinterpret_cast<void**>(last_slot);
            int position[2] = { At<int>(last_tab, tab_pos), MakeRoomInTabBar(menu, options.place) };

            tab_constructor(tab, menu, wide_name.c_str(), position);
            // Nobody walks this tab's children yet, so it can be given room for later containers.
            ReservePointers(tab, tab_childs_begin, container_slots_reserved);
            insert_tab(menu, tab);

            // The constructor leaves texture id -1, so the bar writes the tab's name. A script's texture
            // replaces that and needs its source rectangle too: the whole 48 x 48 texture.
            if (options.icon)
            {
                At<int>(tab, tab_icon_texture_id) = *options.icon;
                At<int>(tab, tab_icon_source) = 0;
                At<int>(tab, tab_icon_source + 4) = 0;
                At<int>(tab, tab_icon_source + 8) = tab_icon_pixels;
                At<int>(tab, tab_icon_source + 12) = tab_icon_pixels;
            }

            if (options.default_columns)
            {
                AddContainer(tab, L"A", Layout(0, 12, 0, tab_blocks));
                AddContainer(tab, L"B", Layout(12, 12, 0, tab_blocks));
            }
            ResetLayout(tab);
            set_visible(tab, 0); // new controls start visible; the builder hides every tab it makes
            script_tabs.push_back(tab); // removed again when the Lua state is closed
            return Status::Created;
        }

        int __cdecl NativeNewTab(void* lua_state) noexcept
        {
            try
            {
                std::string name;
                std::wstring wide_name;
                if (!ReadString(lua_state, 1, name) || !ValidName(name) || !Utf8ToWide(name, wide_name))
                    return Fail(lua_state, "bad argument #1 to 'new_tab' (tab name expected)");
                // Optional 2nd argument: icon (texture id). Optional 3rd: false = empty tab, lay it out
                // with new_container. Before the icon existed the 2nd argument was `columns`, so a
                // boolean there is still read that way. Optional 4th: place in the tab bar.
                TabOptions options;
                const TValue* columns = Argument(lua_state, 3);
                const TValue* second = Argument(lua_state, 2);
                if (second != nullptr && (second->type == itype_false || second->type == itype_true))
                    columns = second;
                else if (!ReadTextureId(lua_state, 2, options.icon))
                    return Fail(lua_state, "bad argument #2 to 'new_tab' (texture id expected)");
                options.default_columns = columns == nullptr || columns->type != itype_false;
                if (!ReadTabPlace(lua_state, 4, options.place))
                    return Fail(lua_state, "bad argument #4 to 'new_tab' (position: number from 1, \"first\" or \"last\" expected)");
                switch (CreateTab(name, wide_name, options))
                {
                case Status::Created: return ReturnBoolean(lua_state, true);
                case Status::Exists: return ReturnBoolean(lua_state, false);
                case Status::NoMenu: return Fail(lua_state, "the menu is not ready yet");
                case Status::NoSlot: return Fail(lua_state, "no free tab slot (menu tab storage was not reserved)");
                default: return Fail(lua_state, "out of memory");
                }
            }
            catch (...)
            {
                return Fail(lua_state, "out of memory");
            }
        }

        int __cdecl NativeNewContainer(void* lua_state) noexcept
        {
            try
            {
                std::string tab_name;
                std::wstring wide_tab_name;
                std::string name;
                std::wstring wide_name;
                if (!ReadString(lua_state, 1, tab_name) || !ValidName(tab_name) || !Utf8ToWide(tab_name, wide_tab_name))
                    return Fail(lua_state, "bad argument #1 to 'new_container' (tab name expected)");
                if (!ReadString(lua_state, 2, name) || !ValidName(name) || !Utf8ToWide(name, wide_name))
                    return Fail(lua_state, "bad argument #2 to 'new_container' (container name expected)");
                int x = 0, width = tab_blocks, y = 0, height = tab_blocks;
                if (!ReadBlocks(lua_state, 3, x) || !ReadBlocks(lua_state, 4, width)
                    || !ReadBlocks(lua_state, 5, y) || !ReadBlocks(lua_state, 6, height))
                    return Fail(lua_state, "bad argument #3-#6 to 'new_container' (number from 0 to 24 expected)");

                void* menu = Menu();
                if (menu == nullptr) return Fail(lua_state, "the menu is not ready yet");
                void* tab = FindTab(menu, wide_tab_name);
                if (tab == nullptr) return Fail(lua_state, "tab not found");
                if (FindContainer(tab, wide_name) != nullptr) return ReturnBoolean(lua_state, false);
                if (SpareSlots(tab, tab_childs_begin) < 1)
                    return Fail(lua_state, "no free container slot in this tab (only tabs made by new_tab have spare room)");
                if (AddContainer(tab, wide_name, Layout(x, width, y, height)) == nullptr)
                    return Fail(lua_state, "out of memory");
                ResetLayout(tab);
                return ReturnBoolean(lua_state, true);
            }
            catch (...)
            {
                return Fail(lua_state, "out of memory");
            }
        }
    }

    bool Install() noexcept
    {
        std::scoped_lock lock(hook_state.mutex);
        if (hook_state.installed) return true;
        if (!HasBytes(tab_switch_address, tab_switch_prologue.data(), tab_switch_prologue.size())) return false;
        if (!InstallHook(hook_state.teardown_hook, lua_teardown_address, lua_teardown_prologue,
                reinterpret_cast<const void*>(&LuaTeardownHook), hook_state.original_teardown))
            return false;
        hook_state.installed = true;
        return true;
    }

    void Remove() noexcept
    {
        std::scoped_lock lock(hook_state.mutex);
        if (!hook_state.installed) return;
        RemoveHook(hook_state.teardown_hook);
        hook_state.original_teardown = nullptr;
        hook_state.installed = false;
    }

    bool IsInstalled() noexcept
    {
        std::scoped_lock lock(hook_state.mutex);
        return hook_state.installed;
    }

    bool ReserveTabSlots(void* menu) noexcept
    {
        if (menu == nullptr || !IsReadable(menu, menu_tabs_end + sizeof(void*) + sizeof(void*))) return false;
        const auto* first = At<std::uint8_t*>(menu, menu_tabs_begin);
        const auto* last = At<std::uint8_t*>(menu, menu_tabs_end);
        // A built menu has its tabs; refuse anything that does not look like it.
        if (first == nullptr || last <= first || static_cast<std::size_t>(last - first) / sizeof(void*) > 64)
            return false;
        if (!ReservePointers(menu, menu_tabs_begin, tab_slots_reserved)) return false;
        known_menu = menu;
        return true;
    }

    void RegisterLuaApi(void* lua_state) noexcept
    {
        if (find_table(lua_state, lua_registry_index, "_LOADED._G.skeetsdk", 4) != nullptr) return;
        // Runs on every load and the GC is off, so do not allocate the closures again every time.
        get_field(lua_state, -1, "new_tab");
        const bool registered = get_type(lua_state, -1) == lua_type_function;
        set_top(lua_state, -2);
        if (registered)
        {
            set_top(lua_state, -2);
            return;
        }
        push_cclosure(lua_state, &NativeNewTab, 0);
        set_field(lua_state, -2, "new_tab");
        push_cclosure(lua_state, &NativeNewContainer, 0);
        set_field(lua_state, -2, "new_container");
        // package.loaded.skeetsdk, so require("skeetsdk") returns it too.
        if (find_table(lua_state, lua_registry_index, "_LOADED", 16) == nullptr)
        {
            push_value(lua_state, -2);
            set_field(lua_state, -2, "skeetsdk");
            set_top(lua_state, -2);
        }
        set_top(lua_state, -2);
    }
#else
    bool Install() noexcept { return false; }
    void Remove() noexcept {}
    bool IsInstalled() noexcept { return false; }
    bool ReserveTabSlots(void*) noexcept { return false; }
    void RegisterLuaApi(void*) noexcept {}
#endif
}
