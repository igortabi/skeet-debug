#include "ui_texture.hpp"

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
#include <optional>
#include <vector>

// A texture control is a label made by the game's own ui.new_label, with its vtable swapped. A label draws its
// name and takes no input, so only two slots change: Render draws the texture instead of the name, and the
// destructor forgets the control before running the label's own. The label's default size becomes the texture's
// and the label's scale handler does the rest; the container stacks its controls by their height, so the
// controls below make room.
//
// ui.new_label (0x4341CF96) appends the label to the container, appends a record holding it to the control
// records, and returns the record's index as the reference; every ui.* function looks references up that way,
// so the wrapper in Lua can make the label and hand its reference straight to _make_texture.

namespace skeetsdk::ui_texture
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
        constexpr std::int32_t itype_nil = -1;
        constexpr std::int32_t itype_true = -3;

        // Menu code (verified in the IDB).
        constexpr std::uintptr_t control_records_address = 0x43479760u;  // {first, last, end} of 0x1C-byte records
        constexpr std::size_t control_record_size = 0x1C;                 // the control at +0 (0 for ui.new_string)
        constexpr std::uintptr_t label_vtable_address = 0x43470008u;     // set by the label constructor 0x43328C61
        constexpr std::size_t label_vtable_slots = 13;
        constexpr std::size_t label_object_size = 0x60;
        constexpr std::size_t vtable_destroy = 0;                         // __thiscall(this, flags), DeleteUi runs it with 1
        constexpr std::size_t vtable_render = 4;                          // __thiscall(this, IRenderer*, const Vec2* origin)
        constexpr std::size_t vtable_apply_scale = 10;                    // label: Size = DefSize at the menu's scale
        constexpr std::size_t vtable_layout = 8;                          // container: place its controls (0x433AD23F)
        constexpr std::size_t renderer_texture_slot = 23;
        constexpr int texture_mode_fill = 1;                              // the whole texture stretched over the size (0x433CCC5C)

        constexpr std::size_t control_parent = 0x10;                      // the container
        constexpr std::size_t control_editable = 0x14;
        constexpr std::size_t control_type = 0x19;
        constexpr std::size_t control_pos = 0x20;                         // set by the container's layout
        constexpr std::size_t control_size = 0x28;                        // the container moves the next control Size.y + 5 down
        constexpr std::size_t label_default_size = 0x30;                  // {200, 12}, scaled into control_size
        constexpr std::uint8_t type_label = 8;
        constexpr int max_texture_pixels = 4096;

        // r | g << 8 | b << 16 | a << 24, as renderer.texture packs it. A label the menu disables draws its text
        // with alpha 70; the texture does the same.
        constexpr std::uint32_t color_enabled = 0xFFFFFFFFu;
        constexpr std::uint32_t color_disabled = 0x46FFFFFFu;

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

        using DestroyFn = void*(__thiscall*)(void*, int);
        using VirtualFn = void(__thiscall*)(void*);
        // IRenderer slot 23, called the way the tab bar (0x433AC6EB) and renderer.texture (0x4336547D) call it:
        // ecx = renderer, edx = texture id, xmm0 = scale, then x, y, color, source x, source y, width, height and
        // mode on the stack, popped by the callee.
        using DrawTextureFn = void(__vectorcall*)(void*, int, int, int, std::uint32_t, int, int, int, int, int, float);

        const auto push_cclosure = reinterpret_cast<PushCClosureFn>(lua_push_cclosure_address);
        const auto set_field = reinterpret_cast<SetFieldFn>(lua_set_field_address);
        const auto push_string = reinterpret_cast<PushStringFn>(lua_push_string_address);
        const auto set_top = reinterpret_cast<SetTopFn>(lua_set_top_address);
        const auto find_table = reinterpret_cast<FindTableFn>(lua_find_table_address);
        const auto get_type = reinterpret_cast<TypeFn>(lua_type_address);
        const auto get_field = reinterpret_cast<GetFieldFn>(lua_get_field_address);
        const auto load_buffer = reinterpret_cast<LoadBufferFn>(lua_load_buffer_address);
        const auto call_protected = reinterpret_cast<PCallFn>(lua_pcall_address);

        // tools/ui_texture_harness.cpp points these at a fake menu to test the control without the game.
#ifdef SKEETSDK_UI_TEXTURE_TEST
#define SKEETSDK_GAME_DATA
#else
#define SKEETSDK_GAME_DATA const
#endif
        SKEETSDK_GAME_DATA std::uintptr_t control_records = control_records_address;
        SKEETSDK_GAME_DATA std::uintptr_t label_vtable = label_vtable_address;

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

        // --- the texture controls -------------------------------------------------------------------

        struct TextureControl
        {
            void* control;
            int texture;
        };

        // The menu draws while holding its container's lock, and DeleteUi destroys a control while holding the
        // same lock, so this one is always taken after the container's and never held while calling the game.
        SRWLOCK registry_lock = SRWLOCK_INIT;
        std::vector<TextureControl> texture_controls;

        // The label's vtable with Render and the destructor replaced, built from the real one on first use.
        // Entry 0 is the label's vtable[-1] (the RTTI locator), so the copy looks like the original from both sides.
        std::array<void*, label_vtable_slots + 1> texture_vtable_storage{};
        bool texture_vtable_ready = false;
        DestroyFn destroy_label = nullptr;

        void* const* TextureVtable() noexcept
        {
            return texture_vtable_storage.data() + 1;
        }

        void* const* VtableOf(void* control) noexcept
        {
            return *static_cast<void* const* const*>(control);
        }

        std::vector<TextureControl>::iterator FindTexture(void* control) noexcept
        {
            return std::find_if(texture_controls.begin(), texture_controls.end(),
                [&](const TextureControl& entry) { return entry.control == control; });
        }

        void __fastcall RenderTexture(void* control, void*, void* renderer, const int* origin) noexcept
        {
            int texture = -1;
            AcquireSRWLockShared(&registry_lock);
            const auto it = FindTexture(control);
            if (it != texture_controls.end()) texture = it->texture;
            ReleaseSRWLockShared(&registry_lock);

            const int width = At<int>(control, control_size);
            const int height = At<int>(control, control_size + 4);
            if (texture < 0 || width <= 0 || height <= 0 || renderer == nullptr || origin == nullptr) return;
            const std::uint32_t color = At<std::uint8_t>(control, control_editable) != 0 ? color_enabled : color_disabled;
            const auto draw = reinterpret_cast<DrawTextureFn>((*static_cast<void***>(renderer))[renderer_texture_slot]);
            draw(renderer, texture, origin[0] + At<int>(control, control_pos), origin[1] + At<int>(control, control_pos + 4),
                color, 0, 0, width, height, texture_mode_fill, 1.0f);
        }

        void* __fastcall DestroyTexture(void* control, void*, int flags) noexcept
        {
            AcquireSRWLockExclusive(&registry_lock);
            const auto it = FindTexture(control);
            if (it != texture_controls.end()) texture_controls.erase(it);
            ReleaseSRWLockExclusive(&registry_lock);
            return destroy_label(control, flags);
        }

        // Under registry_lock (exclusive).
        bool PrepareTextureVtable() noexcept
        {
            if (texture_vtable_ready) return true;
            const auto* label = reinterpret_cast<void* const*>(label_vtable);
            if (!IsReadable(label - 1, (label_vtable_slots + 1) * sizeof(void*))) return false;
            std::copy(label - 1, label + label_vtable_slots, texture_vtable_storage.begin());
            destroy_label = reinterpret_cast<DestroyFn>(label[vtable_destroy]);
            texture_vtable_storage[1 + vtable_destroy] = reinterpret_cast<void*>(&DestroyTexture);
            texture_vtable_storage[1 + vtable_render] = reinterpret_cast<void*>(&RenderTexture);
            texture_vtable_ready = true;
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
            return IsReadable(control, label_object_size) ? control : nullptr;
        }

        // Size at 100% scale. The label's scale handler turns it into the size the container lays out, and the
        // container moves the controls below.
        void Resize(void* control, int width, int height) noexcept
        {
            At<int>(control, label_default_size) = width;
            At<int>(control, label_default_size + 4) = height;
            CallVirtual(control, vtable_apply_scale);
            void* container = At<void*>(control, control_parent);
            if (container != nullptr) CallVirtual(container, vtable_layout);
        }

        enum class Status { Done, NotLabel, NotTexture, NoVtable, NoMemory };

        Status MakeTexture(void* control, int texture, int width, int height) noexcept
        {
            if (VtableOf(control) != reinterpret_cast<void* const*>(label_vtable) || At<std::uint8_t>(control, control_type) != type_label)
                return Status::NotLabel;
            Status status = Status::Done;
            AcquireSRWLockExclusive(&registry_lock);
            if (!PrepareTextureVtable())
            {
                status = Status::NoVtable;
            }
            else
            {
                try
                {
                    // A control the game freed without its destructor could have left its address behind.
                    const auto it = FindTexture(control);
                    if (it != texture_controls.end()) it->texture = texture;
                    else texture_controls.push_back({ control, texture });
                    // Registered first, so a frame drawn in between finds its texture.
                    *static_cast<void* const**>(control) = TextureVtable();
                }
                catch (...)
                {
                    status = Status::NoMemory;
                }
            }
            ReleaseSRWLockExclusive(&registry_lock);
            if (status == Status::Done) Resize(control, width, height);
            return status;
        }

        Status SetTexture(void* control, int texture, std::optional<std::array<int, 2>> size) noexcept
        {
            if (VtableOf(control) != TextureVtable()) return Status::NotTexture;
            AcquireSRWLockExclusive(&registry_lock);
            const auto it = FindTexture(control);
            const bool found = it != texture_controls.end();
            if (found) it->texture = texture;
            ReleaseSRWLockExclusive(&registry_lock);
            if (!found) return Status::NotTexture;
            if (size) Resize(control, (*size)[0], (*size)[1]);
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

        bool IsAbsent(void* lua_state, int index) noexcept
        {
            const TValue* argument = Argument(lua_state, index);
            return argument == nullptr || argument->type == itype_nil;
        }

        // A whole number in [minimum, maximum].
        bool ReadWhole(void* lua_state, int index, int minimum, int maximum, int& value) noexcept
        {
            const TValue* argument = Argument(lua_state, index);
            if (argument == nullptr || get_type(lua_state, index) != lua_type_number) return false;
            double number = 0.0;
            std::memcpy(&number, argument, sizeof(number));
            if (!std::isfinite(number) || number != std::floor(number) || number < minimum || number > maximum) return false;
            value = static_cast<int>(number);
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

        int Report(void* lua_state, Status status) noexcept
        {
            switch (status)
            {
            case Status::Done: return ReturnTrue(lua_state);
            case Status::NotLabel: return Fail(lua_state, "the reference is not a label made by ui.new_label");
            case Status::NotTexture: return Fail(lua_state, "bad argument #1 to 'set_texture' (reference from ui.new_texture expected)");
            case Status::NoVtable: return Fail(lua_state, "the label's vtable is not where it should be");
            default: return Fail(lua_state, "out of memory");
            }
        }

        constexpr int max_reference = std::numeric_limits<int>::max();
        constexpr int max_texture_id = std::numeric_limits<int>::max();

        // skeetsdk._make_texture(reference, texture_id, width, height): turns the label `reference` (fresh from
        // ui.new_label) into a texture control. With reference nil it only checks the other arguments, which the
        // wrapper does before it makes the label, so a bad call leaves nothing behind.
        int __cdecl NativeMakeTexture(void* lua_state) noexcept
        {
            int texture = 0, width = 0, height = 0;
            if (!ReadWhole(lua_state, 2, 0, max_texture_id, texture))
                return Fail(lua_state, "bad argument #3 to 'new_texture' (texture id expected)");
            if (!ReadWhole(lua_state, 3, 1, max_texture_pixels, width))
                return Fail(lua_state, "bad argument #4 to 'new_texture' (width from 1 to 4096 expected)");
            if (!ReadWhole(lua_state, 4, 1, max_texture_pixels, height))
                return Fail(lua_state, "bad argument #5 to 'new_texture' (height from 1 to 4096 expected)");
            if (IsAbsent(lua_state, 1)) return ReturnTrue(lua_state);
            int reference = 0;
            void* control = nullptr;
            if (!ReadWhole(lua_state, 1, 0, max_reference, reference) || (control = ControlAt(static_cast<unsigned>(reference))) == nullptr)
                return Fail(lua_state, "the label made for the texture has no control");
            return Report(lua_state, MakeTexture(control, texture, width, height));
        }

        // skeetsdk.set_texture(reference, texture_id [, width, height])
        int __cdecl NativeSetTexture(void* lua_state) noexcept
        {
            int reference = 0, texture = 0;
            void* control = nullptr;
            if (!ReadWhole(lua_state, 1, 0, max_reference, reference) || (control = ControlAt(static_cast<unsigned>(reference))) == nullptr)
                return Report(lua_state, Status::NotTexture);
            if (!ReadWhole(lua_state, 2, 0, max_texture_id, texture))
                return Fail(lua_state, "bad argument #2 to 'set_texture' (texture id expected)");
            std::optional<std::array<int, 2>> size;
            if (!IsAbsent(lua_state, 3) || !IsAbsent(lua_state, 4))
            {
                std::array<int, 2> pixels{};
                if (!ReadWhole(lua_state, 3, 1, max_texture_pixels, pixels[0]))
                    return Fail(lua_state, "bad argument #3 to 'set_texture' (width from 1 to 4096 expected)");
                if (!ReadWhole(lua_state, 4, 1, max_texture_pixels, pixels[1]))
                    return Fail(lua_state, "bad argument #4 to 'set_texture' (height from 1 to 4096 expected)");
                size = pixels;
            }
            return Report(lua_state, SetTexture(control, texture, size));
        }

        // ui.new_texture makes a label with the game's ui.new_label and hands it to _make_texture; ui.set sends the
        // references it made to set_texture and everything else to the game's ui.set. Written in Lua so that errors,
        // argument passing and return values are exactly the language's, with nothing to unwind in native code.
        constexpr char ui_wrapper_source[] = R"LUA(
local sdk = skeetsdk
if sdk._texture_wrapped then return end
if type(ui) ~= "table" or type(ui.new_label) ~= "function" or type(ui.set) ~= "function" then return end
local new_label, set, set_visible = ui.new_label, ui.set, ui.set_visible
local make, set_texture = sdk._make_texture, sdk.set_texture
local textures = {}
sdk.new_texture = function(tab, container, id, width, height)
    local ok, message = make(nil, id, width, height)
    if not ok then return nil, message end
    local made, item = pcall(new_label, tab, container, "\n")
    if not made then return nil, item end
    ok, message = make(item, id, width, height)
    if not ok then
        pcall(set_visible, item, false)
        return nil, message
    end
    textures[item] = true
    return item
end
ui.new_texture = function(tab, container, id, width, height)
    local item, message = sdk.new_texture(tab, container, id, width, height)
    if item == nil then error(message, 2) end
    return item
end
ui.set = function(item, ...)
    if textures[item] then
        local ok, message = set_texture(item, ...)
        if not ok then error(message, 2) end
        return
    end
    return set(item, ...)
end
sdk._texture_wrapped = true
)LUA";

        bool installing_wrapper = false;

        // Runs on every load until it has worked once in this Lua state (ui may not exist yet on the first loads).
        // luaL_loadbuffer goes through the Lua guard's lua_loadx hook, which calls RegisterLuaApi again, hence the flag.
        void InstallUiWrapper(void* lua_state) noexcept
        {
            if (installing_wrapper) return;
            if (find_table(lua_state, lua_registry_index, "_LOADED._G.skeetsdk", 4) != nullptr) return;
            get_field(lua_state, -1, "_texture_wrapped");
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
        // Runs on every load and the GC is off, so do not allocate the closures again every time.
        get_field(lua_state, -1, "set_texture");
        const bool registered = get_type(lua_state, -1) == lua_type_function;
        set_top(lua_state, -2);
        if (!registered)
        {
            push_cclosure(lua_state, &NativeMakeTexture, 0);
            set_field(lua_state, -2, "_make_texture");
            push_cclosure(lua_state, &NativeSetTexture, 0);
            set_field(lua_state, -2, "set_texture");
        }
        set_top(lua_state, -2);
        InstallUiWrapper(lua_state);
    }
#else
    void RegisterLuaApi(void*) noexcept {}
#endif
}
