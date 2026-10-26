#include "esp_dpi.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Unknwn.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>

namespace skeetsdk::esp_dpi
{
#if defined(_M_IX86)
    namespace
    {
        constexpr std::uintptr_t dpi_scale_address = 0x4346D490u;        // float: the scale everything reads
        // Before the loop the frame also stores round(scale * 350) in a local and hands the features a pointer to it
        // (lea edx, [esp+10h] in the loop; paint_ui gets the same local as [esp+8] after it): what they size by.
        constexpr float frame_scale_unit = 350.0f;                       // the float at 0x43471F04

        // Text is sized when the overlay is replayed on the render thread, by the backend's scale (renderer+8, float
        // at +0x100; vtable 0x43472A80 slot 32 sets it). The frame records commands into [renderer+0x64]: a u32
        // header (type << 27 | offset of the next command) and the data, from 0x433AB0FA(renderer, type, size). The
        // replay (0x4338ACAE) walks [renderer+0x5C] and calls handlers[type](renderer + 4, data) from the 18-entry
        // table at 0x4346D690. Two commands of type 0 with a magic in their data bracket the ESP's commands; the
        // wrapper put in the table's slot 0 sets the backend's scale for them on the render thread, and hands every
        // real type 0 command to the original handler.
        constexpr std::uintptr_t renderer_pointer_address = 0x4347690Cu;
        constexpr std::uintptr_t record_command_address = 0x433AB0FAu;   // __fastcall(renderer, type, size) -> data
        constexpr std::uintptr_t command_handlers_address = 0x4346D690u;
        constexpr int bracket_command_type = 0;
        constexpr std::uintptr_t bracket_command_original = 0x433F0C5Du; // what the table's slot 0 holds
        constexpr std::uint32_t bracket_magic[2]{ 0x5D7A1C93u, 0xE5D1A0C7u };

        struct BracketCommand
        {
            std::uint32_t magic[2];
            float scale;   // > 0: begin, the ESP scale; 0: end
        };

        using RecordCommandFn = void*(__fastcall*)(void*, int, std::size_t);
        using CommandHandlerFn = void(__fastcall*)(void*, const void*);
        // push ebx / mov ebx, [ecx+64h] / push ebp / push esi / push edi
        constexpr std::array<std::uint8_t, 8> record_command_prologue{ 0x53, 0x8B, 0x59, 0x64, 0x55, 0x56, 0x57, 0x8B };

        // The cheat's frame (0x433AD576): the in-game features loop begins at 0x433AD5FC with
        // mov esi, [0x43475A9C]; every path after it reaches 0x433AD626, movss xmm0, [0x43470514].
        constexpr std::uintptr_t loop_begin_address = 0x433AD5FCu;
        constexpr std::uintptr_t after_loop_address = 0x433AD626u;
        constexpr std::array<std::uint8_t, 6> loop_begin_code{ 0x8B, 0x35, 0x9C, 0x5A, 0x47, 0x43 };
        constexpr std::array<std::uint8_t, 8> after_loop_code{ 0xF3, 0x0F, 0x10, 0x05, 0x14, 0x05, 0x47, 0x43 };

        // Lua C API in the mapped image (same addresses the Lua guard uses).
        constexpr std::uintptr_t lua_push_cclosure_address = 0x43432F73u;
        constexpr std::uintptr_t lua_set_field_address = 0x43435B37u;
        constexpr std::uintptr_t lua_set_top_address = 0x43426BDFu;
        constexpr std::uintptr_t lua_find_table_address = 0x43426632u;
        constexpr std::uintptr_t lua_type_address = 0x4343654Au;
        constexpr std::uintptr_t lua_get_field_address = 0x4342EBA2u;
        constexpr std::uintptr_t lua_load_buffer_address = 0x43426D2Fu;
        constexpr std::uintptr_t lua_pcall_address = 0x4342D9B9u;
        constexpr int lua_registry_index = -10000;
        constexpr int lua_type_number = 3;
        constexpr int lua_type_function = 6;

        using LuaCFunction = int(__cdecl*)(void*);
        using PushCClosureFn = void(__cdecl*)(void*, LuaCFunction, int);
        using SetFieldFn = void(__cdecl*)(void*, int, const char*);
        using SetTopFn = void(__cdecl*)(void*, int);
        using FindTableFn = const char*(__cdecl*)(void*, int, const char*, int);
        using TypeFn = int(__cdecl*)(void*, int);
        using GetFieldFn = void(__cdecl*)(void*, int, const char*);
        using LoadBufferFn = int(__cdecl*)(void*, const char*, std::size_t, const char*);
        using PCallFn = int(__cdecl*)(void*, int, int, int);

        const auto push_cclosure = reinterpret_cast<PushCClosureFn>(lua_push_cclosure_address);
        const auto set_field = reinterpret_cast<SetFieldFn>(lua_set_field_address);
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

        std::mutex install_mutex;
        bool installed = false;

        // 0: same as the menu. Set from Lua, read on the game's thread in the frame.
        std::atomic<float> esp_scale{ 1.0f };

        // What BeginEsp replaced, for EndEsp.
        bool swapped = false;
        std::int32_t* swapped_frame_scale = nullptr;
        std::int32_t saved_frame_scale = 0;
        float saved_scale = 0.0f;

        bool IsReadable(const void* address, std::size_t size) noexcept
        {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(address, &info, sizeof(info)) == 0) return false;
            if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
            const auto end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
            return reinterpret_cast<std::uintptr_t>(address) + size <= end;
        }

        // --- render thread: the replay ----------------------------------------------------------------------

        bool bracket_ready = false;          // the wrapper is in the table

        // Counters for skeetsdk._esp_dpi_stats(): where the in-game drawing really is, and whether the replay sees
        // the brackets.
        struct Stats
        {
            std::atomic<std::uint32_t> loops{ 0 };            // times the loop began with a scale
            std::atomic<std::uint32_t> inside_bytes{ 0 };     // commands recorded inside the loop, last frame
            std::atomic<std::uint32_t> frame_bytes{ 0 };      // all commands recorded up to after the loop, last frame
            std::atomic<std::uint32_t> replayed_begins{ 0 };
            std::atomic<std::uint32_t> replayed_ends{ 0 };
            std::atomic<float> replay_scale{ 0.0f };          // what the last begin set
            std::atomic<float> replay_previous{ 0.0f };       // what it replaced
        };
        Stats stats;
        std::uint32_t loop_start_bytes = 0;

        std::uint32_t RecordedBytes() noexcept
        {
            auto* renderer = *reinterpret_cast<std::uint8_t* const*>(renderer_pointer_address);
            if (renderer == nullptr) return 0;
            auto* recording = *reinterpret_cast<std::uint8_t* const*>(renderer + 0x64);
            if (recording == nullptr) return 0;
            const auto begin = *reinterpret_cast<const std::uintptr_t*>(recording + 4);
            const auto end = *reinterpret_cast<const std::uintptr_t*>(recording + 8);
            return end >= begin ? static_cast<std::uint32_t>(end - begin) : 0;
        }
        // The backend's fonts (the scaled-font rebuild 0x433C95CE, which runs when the wanted scale at +0x100 is not
        // the one the fonts were built for, at +0x104): 7 descriptors at backend + i*0x18 ({face, ..., base pixel
        // size at +0x10, parameter at +0x14}); fonts made by create_font(face, trunc(base * scale + 0.5), parameter)
        // into the 12-byte slots {font, texture, ...} of the scaled set at +0x15C, holding the SRW lock at +0xA8
        // exclusively; refresh(backend, slot) uploads a font's glyphs whose font says so (font method +0x10). The game
        // thread holds that lock shared for the whole recording (renderer slots 1 and 2, 0x4339E8D9 / 0x434183E4)
        // and measures text with the same fonts meanwhile. Text picks
        // its set with 0x434172EC: +0x108 (unscaled) when +0x104 is 1.0 or the text asks for it (flag 0x100),
        // else +0x15C. So the ESP's text gets fonts of its own: made here at the ESP scale, swapped into +0x15C with
        // +0x104 set to the ESP scale for the ESP's commands, and swapped back after them.
        constexpr int font_count = 7;
        constexpr std::size_t font_descriptor_size = 0x18;
        constexpr std::size_t font_descriptor_base_size = 0x10;
        constexpr std::size_t font_descriptor_parameter = 0x14;
        constexpr std::size_t font_built_scale_offset = 0x104;
        constexpr std::size_t scaled_fonts_offset = 0x15C;
        constexpr std::size_t font_lock_offset = 0xA8;
        constexpr std::uintptr_t create_font_address = 0x4334E194u;    // __fastcall(face, size, parameter) -> font
        constexpr std::uintptr_t refresh_font_address = 0x4334A887u;   // __fastcall(backend, slot)
        // push ebp / mov ebp, esp
        constexpr std::array<std::uint8_t, 3> create_font_prologue{ 0x55, 0x8B, 0xEC };
        constexpr std::array<std::uint8_t, 8> refresh_font_prologue{ 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14, 0x33, 0xC0 };

        struct FontSlot
        {
            void* font;
            IUnknown* texture;
            std::uint32_t extra;
        };
        static_assert(sizeof(FontSlot) == 12);
        using FontSet = std::array<FontSlot, font_count>;
        using CreateFontFn = void*(__fastcall*)(void*, int, int);
        using RefreshFontFn = void(__fastcall*)(void*, FontSlot*);
        using FontReleaseFn = void(__thiscall*)(void*);
        using FontDirtyFn = bool(__thiscall*)(void*);

        bool fonts_usable = false;           // create_font and refresh are what they should be
        bool text_wrapper_ready = false;     // slot 10 of the handler table has TextHandler
        FontSet esp_fonts{};                 // render thread only
        float esp_fonts_scale = 0.0f;
        bool esp_fonts_failed = false;

        template <typename Fn>
        Fn Method(void* object, std::size_t offset) noexcept
        {
            return *reinterpret_cast<Fn*>(*static_cast<std::uint8_t**>(object) + offset);
        }

        void ReleaseFonts(FontSet& fonts) noexcept
        {
            for (FontSlot& slot : fonts)
            {
                if (slot.texture != nullptr) slot.texture->Release();
                if (slot.font != nullptr) Method<FontReleaseFn>(slot.font, 0)(slot.font);
                slot = {};
            }
        }

        // The cheat's own steps for its scaled set, at the ESP scale; the caller holds the lock. On failure the old
        // set stays (the cheat ends the process when create_font fails; here the ESP just keeps the menu's fonts).
        bool BuildEspFonts(std::uint8_t* backend, float scale) noexcept
        {
            FontSet fonts{};
            bool ok = true;
            for (int i = 0; i < font_count && ok; ++i)
            {
                std::uint8_t* descriptor = backend + i * font_descriptor_size;
                void* face = *reinterpret_cast<void**>(descriptor);
                const int base = *reinterpret_cast<const int*>(descriptor + font_descriptor_base_size);
                const int parameter = *reinterpret_cast<const int*>(descriptor + font_descriptor_parameter);
                const int size = static_cast<int>(static_cast<float>(base) * scale + 0.5f);
                fonts[i].font = reinterpret_cast<CreateFontFn>(create_font_address)(face, size, parameter);
                fonts[i].extra = reinterpret_cast<const FontSlot*>(backend + scaled_fonts_offset)[i].extra;
                ok = fonts[i].font != nullptr;
            }
            if (!ok)
            {
                ReleaseFonts(fonts);
                return false;
            }
            ReleaseFonts(esp_fonts);
            esp_fonts = fonts;
            esp_fonts_scale = scale;
            return true;
        }

        void SwapFonts(std::uint8_t* backend) noexcept
        {
            auto* slots = reinterpret_cast<FontSlot*>(backend + scaled_fonts_offset);
            for (int i = 0; i < font_count; ++i) std::swap(slots[i], esp_fonts[i]);
        }

        void RefreshDirtyFonts(std::uint8_t* backend) noexcept
        {
            auto* slots = reinterpret_cast<FontSlot*>(backend + scaled_fonts_offset);
            for (int i = 0; i < font_count; ++i)
                if (slots[i].font != nullptr && Method<FontDirtyFn>(slots[i].font, 0x10)(slots[i].font))
                    reinterpret_cast<RefreshFontFn>(refresh_font_address)(backend, &slots[i]);
        }

        // Glyphs are not made when text is drawn: the text recorder (0x433E4A54) has 0x43342F16 add every character
        // to the font the text will use (font method +4, down the slot's fallback chain at +8), on the game thread,
        // into the set +0x104 picks then, the menu's. So the ESP's fonts would have none: the wrapper in slot 10 of
        // the handler table does the same for each ESP text command, with the ESP's fonts in place, before drawing it.
        constexpr int text_command_type = 10;
        constexpr std::uintptr_t text_command_original = 0x433CC8FCu;
        constexpr std::uintptr_t cache_glyphs_address = 0x43342F16u;   // __thiscall(renderer + 4, span, flags)
        // mov ecx, [ecx+4] / test ecx, ecx / jz
        constexpr std::array<std::uint8_t, 6> cache_glyphs_prologue{ 0x8B, 0x49, 0x04, 0x85, 0xC9, 0x74 };

        struct TextSpan
        {
            const wchar_t* text;
            std::uint32_t length;
        };
        using CacheGlyphsFn = void(__thiscall*)(void*, const TextSpan*, std::uint32_t);

        bool replay_fonts_swapped = false;   // between a begin and an end command, the ESP's fonts in place

        // A text command's data: x, y, color, flags (+0xC), ..., length (u16 at +0x14), the UTF-16 text (+0x16).
        void __fastcall TextHandler(void* object, const void* data) noexcept
        {
            if (replay_fonts_swapped)
            {
                auto* bytes = static_cast<const std::uint8_t*>(data);
                const TextSpan span{ reinterpret_cast<const wchar_t*>(bytes + 0x16), *reinterpret_cast<const std::uint16_t*>(bytes + 0x14) };
                reinterpret_cast<CacheGlyphsFn>(cache_glyphs_address)(object, &span, *reinterpret_cast<const std::uint32_t*>(bytes + 0xC));
                RefreshDirtyFonts(*reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(object) + 4));
            }
            reinterpret_cast<CommandHandlerFn>(text_command_original)(object, data);
        }

        // The ESP also measures its text while recording (the name's height puts it above the box) and caches its
        // glyphs then, with the set 0x434172EC picks (its only callers are the slot lookup 0x4332AF73 and
        // 0x43342F16). Its first instruction jumps to FontSetStub, which hands the game thread the ESP's set while
        // it records the ESP. The game thread holds the backend's lock shared meanwhile, and the ESP's fonts only
        // change under it exclusively, so they stay put.
        constexpr std::uintptr_t font_set_address = 0x434172ECu;
        constexpr std::uintptr_t font_set_resume_address = 0x434172F4u;
        // test dword ptr [esp+4], 100h
        constexpr std::array<std::uint8_t, 8> font_set_code{ 0xF7, 0x44, 0x24, 0x04, 0x00, 0x01, 0x00, 0x00 };
        constexpr std::size_t unscaled_fonts_offset = 0x108;
        constexpr std::uint32_t unscaled_font_flag = 0x100;

        std::atomic<DWORD> recording_esp_thread{ 0 };   // the game thread between BeginEsp and EndEsp
        float recording_esp_scale = 0.0f;

        // 0: pick as the cheat does.
        void* __cdecl SelectFontSet(std::uint8_t* backend, std::uint32_t flags) noexcept
        {
            if ((flags & unscaled_font_flag) != 0) return nullptr;
            if (recording_esp_thread.load(std::memory_order_relaxed) != GetCurrentThreadId()) return nullptr;
            if (recording_esp_scale == 1.0f) return backend + unscaled_fonts_offset;
            if (text_wrapper_ready && esp_fonts_scale == recording_esp_scale) return esp_fonts.data();
            return nullptr; // not built yet: the first replay at this scale builds them
        }

        __declspec(naked) void FontSetStub()
        {
            __asm
            {
                push ecx
                push edx
                push dword ptr [esp + 0x0C]
                push ecx
                call SelectFontSet
                add esp, 8
                pop edx
                pop ecx
                test eax, eax
                jz pick
                ret 4
            pick:
                test dword ptr [esp + 4], 0x100
                push 0x434172F4 // font_set_resume_address
                ret
            }
        }

        // Font metrics skip 0x434172EC: the secondary vtable (0x4346F46C) slots 34 and 35 (0x4334113A, a font's line
        // height, which puts the ESP's name above the box, and 0x4332917E) take slot i of the set +0x104 picks from
        // 0x4336C370, and slot 33 (0x4335BCB2) returns +0x104 itself. 0x4336C370's first instruction jumps to
        // FontSlotStub, and slot 33 gets FontScaleStub, so they also see the ESP's set while it is recorded.
        constexpr std::uintptr_t font_slot_address = 0x4336C370u;      // __thiscall(backend, index) -> slot
        // movss xmm0, [ecx+104h]
        constexpr std::array<std::uint8_t, 8> font_slot_code{ 0xF3, 0x0F, 0x10, 0x81, 0x04, 0x01, 0x00, 0x00 };
        constexpr std::uintptr_t font_scale_method_address = 0x4346F4F0u;
        constexpr std::uintptr_t font_scale_original = 0x4335BCB2u;

        __declspec(naked) void FontSlotStub()
        {
            __asm
            {
                push ecx
                push 0
                push ecx
                call SelectFontSet
                add esp, 8
                pop ecx
                test eax, eax
                jz pick
                imul edx, dword ptr [esp + 4], 0x0C
                add eax, edx
                ret 4
            pick:
                movss xmm0, dword ptr [ecx + 0x104]
                push 0x4336C378 // font_slot_address + 8
                ret
            }
        }

        // The scale of the fonts SelectFontSet hands out, or false.
        bool __cdecl SelectedFontScale(float* scale) noexcept
        {
            if (recording_esp_thread.load(std::memory_order_relaxed) != GetCurrentThreadId()) return false;
            if (recording_esp_scale != 1.0f && !(text_wrapper_ready && esp_fonts_scale == recording_esp_scale)) return false;
            *scale = recording_esp_scale;
            return true;
        }

        // The original: mov eax, [ecx+4] / movss xmm0, [eax+104h] / ret.
        __declspec(naked) void FontScaleStub()
        {
            __asm
            {
                push ecx
                push edx
                sub esp, 4
                push esp
                call SelectedFontScale
                add esp, 4
                movss xmm0, dword ptr [esp]
                add esp, 4
                pop edx
                pop ecx
                test al, al
                jnz done
                mov eax, dword ptr [ecx + 4]
                movss xmm0, dword ptr [eax + 0x104]
            done:
                ret
            }
        }

        bool replay_scaled = false;          // between a begin and an end command, holding the backend's lock
        SRWLOCK* replay_lock = nullptr;
        float replay_saved_built_scale = 0.0f;

        void __fastcall BracketHandler(void* object, const void* data) noexcept
        {
            const auto* command = static_cast<const BracketCommand*>(data);
            if (command->magic[0] != bracket_magic[0] || command->magic[1] != bracket_magic[1])
            {
                reinterpret_cast<CommandHandlerFn>(bracket_command_original)(object, data);
                return;
            }
            // object is renderer + 4; the backend is [object + 4] (what vtable slot 32 writes through).
            auto* backend = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(object) + 4);
            if (backend == nullptr) return;
            auto& built_scale = *reinterpret_cast<float*>(backend + font_built_scale_offset);
            if (command->scale > 0.0f && !replay_scaled)
            {
                // Like the cheat's own rebuild: exclusively, so the game thread (which picks fonts by +0x104 and
                // measures text with them) never sees a set being swapped, nor menu text the ESP's fonts. Held until
                // the end command, which the recording always puts after a begin.
                replay_lock = reinterpret_cast<SRWLOCK*>(backend + font_lock_offset);
                AcquireSRWLockExclusive(replay_lock);
                stats.replay_previous.store(built_scale, std::memory_order_relaxed);
                replay_saved_built_scale = built_scale;
                replay_fonts_swapped = false;
                if (command->scale == 1.0f)
                {
                    built_scale = 1.0f; // text takes the unscaled set at +0x108
                }
                else if (fonts_usable && text_wrapper_ready && !esp_fonts_failed)
                {
                    if (esp_fonts_scale != command->scale && !BuildEspFonts(backend, command->scale)) esp_fonts_failed = true;
                    if (esp_fonts_scale == command->scale)
                    {
                        SwapFonts(backend);
                        replay_fonts_swapped = true;
                        built_scale = command->scale;
                        RefreshDirtyFonts(backend);
                    }
                }
                replay_scaled = true;
                stats.replayed_begins.fetch_add(1, std::memory_order_relaxed);
                stats.replay_scale.store(built_scale, std::memory_order_relaxed);
            }
            else if (command->scale <= 0.0f && replay_scaled)
            {
                built_scale = replay_saved_built_scale;
                if (replay_fonts_swapped) SwapFonts(backend);
                ReleaseSRWLockExclusive(replay_lock);
                replay_fonts_swapped = false;
                replay_lock = nullptr;
                replay_scaled = false;
                stats.replayed_ends.fetch_add(1, std::memory_order_relaxed);
            }
        }

        // --- game thread: the recording ---------------------------------------------------------------------

        bool RecordBracket(float scale) noexcept
        {
            if (!bracket_ready) return false;
            void* renderer = *reinterpret_cast<void* const*>(renderer_pointer_address);
            if (renderer == nullptr) return false;
            auto* command = static_cast<BracketCommand*>(
                reinterpret_cast<RecordCommandFn>(record_command_address)(renderer, bracket_command_type, sizeof(BracketCommand)));
            if (command == nullptr) return false;
            command->magic[0] = bracket_magic[0];
            command->magic[1] = bracket_magic[1];
            command->scale = scale;
            return true;
        }

        bool bracket_open = false;

        void __cdecl BeginEsp(std::int32_t* frame_scale) noexcept
        {
            swapped = false;
            bracket_open = false;
            const float scale = esp_scale.load(std::memory_order_relaxed);
            if (!(scale > 0.0f)) return;
            // Sizes are worked out while recording: the frame's value and the global...
            saved_frame_scale = *frame_scale;
            *frame_scale = static_cast<std::int32_t>(std::lrintf(scale * frame_scale_unit));
            swapped_frame_scale = frame_scale;
            auto& global = *reinterpret_cast<float*>(dpi_scale_address);
            saved_scale = global;
            global = scale;
            swapped = true;
            // ...text when replaying, so that goes through the command stream (changing the backend's scale here
            // raced the render thread and made the menu flicker).
            bracket_open = RecordBracket(scale);
            if (bracket_open)
            {
                recording_esp_scale = scale;
                recording_esp_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
            }
            loop_start_bytes = RecordedBytes();
            stats.loops.fetch_add(1, std::memory_order_relaxed);
        }

        void __cdecl EndEsp(std::int32_t* frame_scale) noexcept
        {
            if (swapped)
            {
                const std::uint32_t now = RecordedBytes();
                stats.inside_bytes.store(now >= loop_start_bytes ? now - loop_start_bytes : 0, std::memory_order_relaxed);
                stats.frame_bytes.store(now, std::memory_order_relaxed);
            }
            recording_esp_thread.store(0, std::memory_order_relaxed);
            if (bracket_open) RecordBracket(0.0f);
            bracket_open = false;
            if (!swapped) return;
            swapped = false;
            if (frame_scale == swapped_frame_scale) *frame_scale = saved_frame_scale;
            *reinterpret_cast<float*>(dpi_scale_address) = saved_scale;
        }

        // Called in place of the two instructions: keep every register, hand over the frame's scale local, then do
        // what the instruction did. At 0x433AD5FC the local is [esp+0Ch] (the frame pushed ecx for it, then edi, esi
        // and ebp), so [esp+10h] past the return address and [esp+34h] past pushad and pushfd; at 0x433AD626 it is
        // [esp+8] again on every path (ebx and ebp are popped), so [esp+30h] here.
        __declspec(naked) void LoopBeginStub()
        {
            __asm
            {
                pushad
                pushfd
                lea eax, [esp + 0x34]
                push eax
                call BeginEsp
                add esp, 4
                popfd
                popad
                mov esi, dword ptr ds:[0x43475A9C]
                ret
            }
        }

        __declspec(naked) void AfterLoopStub()
        {
            __asm
            {
                pushad
                pushfd
                lea eax, [esp + 0x30]
                push eax
                call EndEsp
                add esp, 4
                popfd
                popad
                movss xmm0, dword ptr ds:[0x43470514]
                ret
            }
        }

        // E8 (call) or E9 (jmp) to the stub, the rest of the instruction nops.
        template <std::size_t N>
        bool PatchCall(std::uintptr_t address, const std::array<std::uint8_t, N>& expected, const void* stub,
            std::uint8_t opcode = 0xE8) noexcept
        {
            static_assert(N >= 5);
            auto* target = reinterpret_cast<std::uint8_t*>(address);
            if (!IsReadable(target, N) || std::memcmp(target, expected.data(), N) != 0) return false;
            std::array<std::uint8_t, N> code{};
            code.fill(0x90);
            code[0] = opcode;
            const auto relative = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(stub) - (address + 5));
            std::memcpy(code.data() + 1, &relative, sizeof(relative));
            DWORD old_protection{};
            if (!VirtualProtect(target, N, PAGE_EXECUTE_READWRITE, &old_protection)) return false;
            std::memcpy(target, code.data(), N);
            DWORD ignored{};
            VirtualProtect(target, N, old_protection, &ignored);
            FlushInstructionCache(GetCurrentProcess(), target, N);
            return true;
        }

        template <std::size_t N>
        void Restore(std::uintptr_t address, const std::array<std::uint8_t, N>& original) noexcept
        {
            auto* target = reinterpret_cast<std::uint8_t*>(address);
            DWORD old_protection{};
            if (!VirtualProtect(target, N, PAGE_EXECUTE_READWRITE, &old_protection)) return;
            std::memcpy(target, original.data(), N);
            DWORD ignored{};
            VirtualProtect(target, N, old_protection, &ignored);
            FlushInstructionCache(GetCurrentProcess(), target, N);
        }

        // skeetsdk._set_esp_dpi(scale): 1 to 2, or 0 for the menu's scale.
        int __cdecl NativeSetEspDpi(void* lua_state) noexcept
        {
            const auto state = static_cast<const std::uint8_t*>(lua_state);
            const auto base = *reinterpret_cast<TValue* const*>(state + 0x10);
            const auto top = *reinterpret_cast<TValue* const*>(state + 0x14);
            if (base == nullptr || top - base < 1 || get_type(lua_state, 1) != lua_type_number) return 0;
            double scale = 0.0;
            std::memcpy(&scale, base, sizeof(scale));
            if (!std::isfinite(scale) || scale < 0.0 || scale > 4.0) return 0;
            esp_scale.store(static_cast<float>(scale), std::memory_order_relaxed);
            return 0;
        }

        // skeetsdk._esp_dpi_stats() -> installed, replay wrapper in, loops, bytes inside the loop, bytes up to after
        // it, begins replayed, ends replayed, font scale used for the ESP, the one it replaced, font functions
        // found, scale the ESP fonts were built for, building them failed
        int __cdecl NativeStats(void* lua_state) noexcept
        {
            using PushNumberFn = void(__cdecl*)(void*, double);
            const auto push_number = reinterpret_cast<PushNumberFn>(0x4343525Fu);
            push_number(lua_state, installed ? 1 : 0);
            push_number(lua_state, bracket_ready ? 1 : 0);
            push_number(lua_state, stats.loops.load());
            push_number(lua_state, stats.inside_bytes.load());
            push_number(lua_state, stats.frame_bytes.load());
            push_number(lua_state, stats.replayed_begins.load());
            push_number(lua_state, stats.replayed_ends.load());
            push_number(lua_state, stats.replay_scale.load());
            push_number(lua_state, stats.replay_previous.load());
            push_number(lua_state, fonts_usable && text_wrapper_ready ? 1 : 0);
            push_number(lua_state, esp_fonts_scale);
            push_number(lua_state, esp_fonts_failed ? 1 : 0);
            return 12;
        }

        // The control, under the cheat's own DPI scale. Every new Lua state makes it again, with the last choice from
        // skeetsdk.persist (a config loaded later sets it too); its value is the native scale's only source.
        constexpr char wrapper_source[] = R"LUA(
local sdk = skeetsdk
if sdk._esp_dpi_installed ~= nil then return end
if type(ui) ~= "table" or type(ui.new_combobox) ~= "function" then return end
sdk._esp_dpi_installed = true
local set = sdk._set_esp_dpi
local scales = { ["100%"] = 1, ["125%"] = 1.25, ["150%"] = 1.5, ["175%"] = 1.75, ["200%"] = 2, ["Same as menu"] = 0 }
local control = ui.new_combobox("MISC", "Settings", "ESP DPI scale", "100%", "125%", "150%", "175%", "200%", "Same as menu")
-- Every script load or unload makes the control again: bring the choice back from before.
local memory = type(sdk.persist) == "table" and sdk.persist("esp_dpi") or nil
if memory ~= nil and scales[memory.choice] ~= nil then ui.set(control, memory.choice) end
local function apply()
    local choice = ui.get(control)
    set(scales[choice] or 1)
    if memory ~= nil then memory.choice = choice end
end
ui.set_callback(control, apply)
apply()
)LUA";

        bool installing_wrapper = false;

        void InstallWrapper(void* lua_state) noexcept
        {
            if (installing_wrapper) return;
            if (find_table(lua_state, lua_registry_index, "_LOADED._G.skeetsdk", 4) != nullptr) return;
            get_field(lua_state, -1, "_esp_dpi_installed");
            const bool done = get_type(lua_state, -1) != 0;
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

    bool Install() noexcept
    {
        std::scoped_lock lock(install_mutex);
        if (installed) return true;
        if (!PatchCall(loop_begin_address, loop_begin_code, reinterpret_cast<const void*>(&LoopBeginStub))) return false;
        if (!PatchCall(after_loop_address, after_loop_code, reinterpret_cast<const void*>(&AfterLoopStub)))
        {
            Restore(loop_begin_address, loop_begin_code);
            return false;
        }
        installed = true;

        fonts_usable = IsReadable(reinterpret_cast<const void*>(create_font_address), create_font_prologue.size())
            && std::memcmp(reinterpret_cast<const void*>(create_font_address), create_font_prologue.data(), create_font_prologue.size()) == 0
            && IsReadable(reinterpret_cast<const void*>(refresh_font_address), refresh_font_prologue.size())
            && std::memcmp(reinterpret_cast<const void*>(refresh_font_address), refresh_font_prologue.data(), refresh_font_prologue.size()) == 0
            && IsReadable(reinterpret_cast<const void*>(cache_glyphs_address), cache_glyphs_prologue.size())
            && std::memcmp(reinterpret_cast<const void*>(cache_glyphs_address), cache_glyphs_prologue.data(), cache_glyphs_prologue.size()) == 0;

        // The replay side: wrap the table's slots 0 and 10 if they hold what they should (sizes still follow the ESP
        // scale without them, text does not).
        auto* handlers = reinterpret_cast<std::uintptr_t*>(command_handlers_address);
        constexpr std::size_t handlers_size = (text_command_type + 1) * sizeof(std::uintptr_t);
        if (IsReadable(handlers, handlers_size) && handlers[bracket_command_type] == bracket_command_original
            && IsReadable(reinterpret_cast<const void*>(record_command_address), record_command_prologue.size())
            && std::memcmp(reinterpret_cast<const void*>(record_command_address), record_command_prologue.data(),
                record_command_prologue.size()) == 0)
        {
            DWORD old_protection{};
            if (VirtualProtect(handlers, handlers_size, PAGE_READWRITE, &old_protection))
            {
                handlers[bracket_command_type] = reinterpret_cast<std::uintptr_t>(&BracketHandler);
                bracket_ready = true;
                if (fonts_usable && handlers[text_command_type] == text_command_original)
                {
                    handlers[text_command_type] = reinterpret_cast<std::uintptr_t>(&TextHandler);
                    text_wrapper_ready = true;
                }
                DWORD ignored{};
                VirtualProtect(handlers, handlers_size, old_protection, &ignored);
                // Without it the ESP's text is still drawn at the ESP scale, only measured at the menu's.
                if (text_wrapper_ready)
                {
                    PatchCall(font_set_address, font_set_code, reinterpret_cast<const void*>(&FontSetStub), 0xE9);
                    PatchCall(font_slot_address, font_slot_code, reinterpret_cast<const void*>(&FontSlotStub), 0xE9);
                    auto* method = reinterpret_cast<std::uintptr_t*>(font_scale_method_address);
                    if (IsReadable(method, sizeof(*method)) && *method == font_scale_original
                        && VirtualProtect(method, sizeof(*method), PAGE_READWRITE, &old_protection))
                    {
                        *method = reinterpret_cast<std::uintptr_t>(&FontScaleStub);
                        VirtualProtect(method, sizeof(*method), old_protection, &ignored);
                    }
                }
            }
        }
        return true;
    }

    bool IsInstalled() noexcept
    {
        std::scoped_lock lock(install_mutex);
        return installed;
    }

    void RegisterLuaApi(void* lua_state) noexcept
    {
        if (find_table(lua_state, lua_registry_index, "_LOADED._G.skeetsdk", 4) != nullptr) return;
        // Runs on every load and the GC is off, so do not allocate the closure again every time.
        get_field(lua_state, -1, "_set_esp_dpi");
        const bool registered = get_type(lua_state, -1) == lua_type_function;
        set_top(lua_state, -2);
        if (!registered)
        {
            push_cclosure(lua_state, &NativeSetEspDpi, 0);
            set_field(lua_state, -2, "_set_esp_dpi");
            push_cclosure(lua_state, &NativeStats, 0);
            set_field(lua_state, -2, "_esp_dpi_stats");
        }
        set_top(lua_state, -2);
        InstallWrapper(lua_state);
    }
#else
    bool Install() noexcept { return false; }
    bool IsInstalled() noexcept { return false; }
    void RegisterLuaApi(void*) noexcept {}
#endif
}
