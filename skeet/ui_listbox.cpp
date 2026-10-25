#include "ui_listbox.hpp"

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

// A fitted listbox is one made by the game's own ui.new_listbox (0x434032B6: constructor 0x433EDA39 with width 158,
// height 300 and a search box) whose height is set to its rows. The listbox draws its search box in the first row
// and then min(items, Size.y / row - 1) rows (render 0x434179DE), and turns its scrollbar on when the items it
// shows outnumber those rows (0x4333FF11), so a height of (items + 1) rows shows every item and no scrollbar.
//
// The game sets the height again in two places: the scale handler (vtable slot 10, 0x4339E4DF) recomputes it from
// the default height, and the item setter behind ui.update (0x433C9FBF) keeps the old height for the new items.
// The first is covered by a copy of the listbox vtable with slot 10 wrapped, the second by the Lua wrapper calling
// _fit_listbox again after ui.update.

namespace skeetsdk::ui_listbox
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
        constexpr std::uintptr_t lua_type_address = 0x4343654Au;
        constexpr std::uintptr_t lua_get_field_address = 0x4342EBA2u;
        constexpr std::uintptr_t lua_load_buffer_address = 0x43426D2Fu;  // luaL_loadbuffer(L, buffer, size, name)
        constexpr std::uintptr_t lua_pcall_address = 0x4342D9B9u;        // lua_pcall(L, nargs, nresults, errfunc)
        constexpr int lua_registry_index = -10000;
        constexpr int lua_type_boolean = 1;
        constexpr int lua_type_number = 3;
        constexpr int lua_type_function = 6;
        constexpr std::int32_t itype_true = -3;

        // Menu code (verified in the IDB).
        constexpr std::uintptr_t control_records_address = 0x43479760u;  // {first, last, end} of 0x1C-byte records
        constexpr std::size_t control_record_size = 0x1C;                 // the control at +0; a reference is the index
        constexpr std::uintptr_t listbox_vtable_address = 0x434708F0u;   // set by the listbox constructor 0x433EDA39
        // Slots 13 to 16 may already be the next table's; copying them as well changes nothing either way.
        constexpr std::size_t listbox_vtable_slots = 17;
        constexpr std::size_t listbox_object_size = 0xC0;
        constexpr std::size_t vtable_apply_scale = 10;                    // listbox: size, row height, rows, scrollbar
        constexpr std::size_t vtable_layout = 8;                          // container: place its controls (0x433AD23F)

        constexpr std::size_t control_parent = 0x10;                      // the container
        constexpr std::size_t control_type = 0x19;
        constexpr std::size_t control_size = 0x28;                        // the container moves the next control Size.y + 5 down
        constexpr std::size_t control_default_size = 0x30;                // scaled into control_size by slot 10
        constexpr std::uint8_t type_listbox = 6;

        constexpr std::size_t listbox_row_height = 0x60;                  // 18 at 100%, scaled by slot 10
        constexpr std::size_t listbox_scrollbar = 0x68;                   // byte: scrollbar on
        constexpr std::size_t listbox_search = 0x7C;                      // byte: a search box takes the first row
        constexpr std::size_t listbox_rows = 0x80;                        // rows that fit below the search box
        constexpr std::size_t listbox_scroll = 0x84;                      // pixels scrolled, read only while the scrollbar is on
        constexpr std::size_t listbox_items = 0x90;                       // {first, last, end} of 20-byte items
        constexpr std::size_t listbox_item_size = 20;
        constexpr int listbox_row_pixels = 18;                            // the constructor's row height
        constexpr std::size_t max_fitted_rows = 0x10000;

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
        using TypeFn = int(__cdecl*)(void*, int);
        using GetFieldFn = void(__cdecl*)(void*, int, const char*);
        using LoadBufferFn = int(__cdecl*)(void*, const char*, std::size_t, const char*);
        using PCallFn = int(__cdecl*)(void*, int, int, int);

        using VirtualFn = void(__thiscall*)(void*);

        const auto push_cclosure = reinterpret_cast<PushCClosureFn>(lua_push_cclosure_address);
        const auto set_field = reinterpret_cast<SetFieldFn>(lua_set_field_address);
        const auto push_string = reinterpret_cast<PushStringFn>(lua_push_string_address);
        const auto set_top = reinterpret_cast<SetTopFn>(lua_set_top_address);
        const auto find_table = reinterpret_cast<FindTableFn>(lua_find_table_address);
        const auto get_type = reinterpret_cast<TypeFn>(lua_type_address);
        const auto get_field = reinterpret_cast<GetFieldFn>(lua_get_field_address);
        const auto load_buffer = reinterpret_cast<LoadBufferFn>(lua_load_buffer_address);
        const auto call_protected = reinterpret_cast<PCallFn>(lua_pcall_address);

        // tools/ui_listbox_harness.cpp points these at a fake menu to test the listbox without the game.
#ifdef SKEETSDK_UI_LISTBOX_TEST
#define SKEETSDK_GAME_DATA
#else
#define SKEETSDK_GAME_DATA const
#endif
        SKEETSDK_GAME_DATA std::uintptr_t control_records = control_records_address;
        SKEETSDK_GAME_DATA std::uintptr_t listbox_vtable = listbox_vtable_address;

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

        void CallVirtual(void* object, std::size_t slot) noexcept
        {
            (*reinterpret_cast<VirtualFn**>(object))[slot](object);
        }

        // --- fitting ---------------------------------------------------------------------------------

        // The listbox's vtable with the scale handler wrapped, built from the real one on first use. Entry 0 is the
        // listbox's vtable[-1], so the copy looks like the original from both sides. Only the Lua thread builds it.
        std::array<void*, listbox_vtable_slots + 1> fitted_vtable_storage{};
        bool fitted_vtable_ready = false;
        VirtualFn apply_scale_listbox = nullptr;

        void* const* FittedVtable() noexcept
        {
            return fitted_vtable_storage.data() + 1;
        }

        void* const* VtableOf(void* control) noexcept
        {
            return *static_cast<void* const* const*>(control);
        }

        std::size_t ItemCount(void* listbox) noexcept
        {
            const std::uint8_t* first = At<std::uint8_t*>(listbox, listbox_items);
            const std::uint8_t* last = At<std::uint8_t*>(listbox, listbox_items + sizeof(void*));
            if (first == nullptr || last < first) return 0;
            return static_cast<std::size_t>(last - first) / listbox_item_size;
        }

        // Height for the search box and every item (at least one row), at the row height the listbox has now.
        // The rows below the search box are exactly the items, so the game's scrollbar check would turn it off;
        // it is turned off here, and the scroll offset cleared, without calling back into the game.
        void FitRows(void* listbox) noexcept
        {
            const int row = At<int>(listbox, listbox_row_height);
            if (row <= 0) return;
            const int search = At<std::uint8_t>(listbox, listbox_search) != 0 ? 1 : 0;
            const int rows = static_cast<int>(std::clamp<std::size_t>(ItemCount(listbox), 1, max_fitted_rows));
            At<int>(listbox, control_size + 4) = (rows + search) * row;
            At<int>(listbox, control_default_size + 4) = (rows + search) * listbox_row_pixels;
            At<int>(listbox, listbox_rows) = rows;
            At<std::uint8_t>(listbox, listbox_scrollbar) = 0;
            At<int>(listbox, listbox_scroll) = 0;
        }

        // Slot 10 of a fitted listbox: the game's scale handler, then the height again at the new row height.
        void __fastcall ApplyScaleFitted(void* listbox, void*) noexcept
        {
            apply_scale_listbox(listbox);
            FitRows(listbox);
        }

        bool PrepareFittedVtable() noexcept
        {
            if (fitted_vtable_ready) return true;
            const auto* listbox = reinterpret_cast<void* const*>(listbox_vtable);
            if (!IsReadable(listbox - 1, (listbox_vtable_slots + 1) * sizeof(void*))) return false;
            std::copy(listbox - 1, listbox + listbox_vtable_slots, fitted_vtable_storage.begin());
            apply_scale_listbox = reinterpret_cast<VirtualFn>(listbox[vtable_apply_scale]);
            fitted_vtable_storage[1 + vtable_apply_scale] = reinterpret_cast<void*>(&ApplyScaleFitted);
            fitted_vtable_ready = true;
            return true;
        }

        // The control behind a reference from ui.new_* / ui.reference, or nullptr.
        void* ControlAt(unsigned reference) noexcept
        {
            const auto* records = reinterpret_cast<std::uint8_t* const*>(control_records);
            if (!IsReadable(records, 2 * sizeof(void*))) return nullptr;
            const std::uint8_t* first = records[0];
            const std::uint8_t* last = records[1];
            if (first == nullptr || last < first || reference >= static_cast<std::size_t>(last - first) / control_record_size)
                return nullptr;
            const std::uint8_t* record = first + reference * control_record_size;
            if (!IsReadable(record, sizeof(void*))) return nullptr;
            void* control = *reinterpret_cast<void* const*>(record);
            return IsReadable(control, listbox_object_size) ? control : nullptr;
        }

        enum class Status { Done, NotListbox, NoVtable };

        // Makes a listbox fitted (once) and fits it to the items it has now; the container moves the controls below.
        Status Fit(void* control) noexcept
        {
            const auto* vtable = VtableOf(control);
            if (At<std::uint8_t>(control, control_type) != type_listbox
                || (vtable != reinterpret_cast<void* const*>(listbox_vtable) && vtable != FittedVtable()))
                return Status::NotListbox;
            if (vtable != FittedVtable())
            {
                if (!PrepareFittedVtable()) return Status::NoVtable;
                *static_cast<void* const**>(control) = FittedVtable();
            }
            FitRows(control);
            void* container = At<void*>(control, control_parent);
            if (container != nullptr) CallVirtual(container, vtable_layout);
            return Status::Done;
        }

        // --- Lua ------------------------------------------------------------------------------------

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

        // A reference: a whole number >= 0.
        bool ReadReference(void* lua_state, int index, unsigned& value) noexcept
        {
            const TValue* argument = Argument(lua_state, index);
            if (argument == nullptr || get_type(lua_state, index) != lua_type_number) return false;
            double number = 0.0;
            std::memcpy(&number, argument, sizeof(number));
            if (!std::isfinite(number) || number != std::floor(number) || number < 0.0
                || number > static_cast<double>(std::numeric_limits<int>::max()))
                return false;
            value = static_cast<unsigned>(number);
            return true;
        }

        // Pushes true straight onto the stack (lua_pushboolean is not needed elsewhere).
        // A C function is guaranteed LUA_MINSTACK free slots.
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

        // skeetsdk._fit_listbox(reference): what ui.new_listbox(..., true) and ui.update call on a fitted listbox.
        int __cdecl NativeFitListbox(void* lua_state) noexcept
        {
            unsigned reference = 0;
            void* control = nullptr;
            if (!ReadReference(lua_state, 1, reference) || (control = ControlAt(reference)) == nullptr)
                return Fail(lua_state, "bad argument #1 to '_fit_listbox' (listbox reference expected)");
            switch (Fit(control))
            {
            case Status::Done: return ReturnTrue(lua_state);
            case Status::NotListbox: return Fail(lua_state, "bad argument #1 to '_fit_listbox' (listbox reference expected)");
            default: return Fail(lua_state, "the listbox's vtable is not where it should be");
            }
        }

        // ui.new_listbox passes everything to the game's (which skips arguments that are not strings) and fits the
        // listbox when the last argument, from the 4th on, is true. ui.update fits a fitted listbox again after the
        // game set its items. Written in Lua so that errors, argument passing and return values are exactly the
        // language's, with nothing to unwind in native code.
        constexpr char ui_wrapper_source[] = R"LUA(
local sdk = skeetsdk
if sdk._listbox_wrapped then return end
if type(ui) ~= "table" or type(ui.new_listbox) ~= "function" then return end
local new_listbox, update = ui.new_listbox, ui.update
local fit = sdk._fit_listbox
local fitted = {}
ui.new_listbox = function(...)
    local item = new_listbox(...)
    local count = select("#", ...)
    if count >= 4 and select(count, ...) == true then
        local ok, message = fit(item)
        if not ok then error(message, 2) end
        fitted[item] = true
    end
    return item
end
if type(update) == "function" then
    local function refit(item, ...)
        fit(item)
        return ...
    end
    ui.update = function(item, ...)
        if fitted[item] then return refit(item, update(item, ...)) end
        return update(item, ...)
    end
end
sdk._listbox_wrapped = true
)LUA";

        bool installing_wrapper = false;

        // Runs on every load until it has worked once in this Lua state (ui may not exist yet on the first loads).
        // luaL_loadbuffer goes through the Lua guard's lua_loadx hook, which calls RegisterLuaApi again, hence the flag.
        void InstallUiWrapper(void* lua_state) noexcept
        {
            if (installing_wrapper) return;
            if (find_table(lua_state, lua_registry_index, "_LOADED._G.skeetsdk", 4) != nullptr) return;
            get_field(lua_state, -1, "_listbox_wrapped");
            const bool done = get_type(lua_state, -1) == lua_type_boolean;
            set_top(lua_state, -3);
            if (done) return;
            installing_wrapper = true;
            if (load_buffer(lua_state, ui_wrapper_source, sizeof(ui_wrapper_source) - 1, "=skeetsdk") == 0)
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
        get_field(lua_state, -1, "_fit_listbox");
        const bool registered = get_type(lua_state, -1) == lua_type_function;
        set_top(lua_state, -2);
        if (!registered)
        {
            push_cclosure(lua_state, &NativeFitListbox, 0);
            set_field(lua_state, -2, "_fit_listbox");
        }
        set_top(lua_state, -2);
        InstallUiWrapper(lua_state);
    }
#else
    void RegisterLuaApi(void*) noexcept {}
#endif
}
