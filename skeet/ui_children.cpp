#include "ui_children.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

// The two fillers, as the game has them (both __thiscall(container, ChildList* out), retn 4):
//
//   out->begin = out->end = out->slots; out->capacity = out->slots + 1024;
//   for (child : container->children)
//       if (input || child->visible) { *out->end++ = child; if (count >= 1024 || begin != slots) break; }
//   sort(out->begin, out->end, comparator);   // __fastcall(begin, end, comparator), retn 4
//
// The replacements do the same, with the list moved to a buffer of their own when it would not fit.

namespace skeetsdk::ui_children
{
#if defined(_M_IX86)
    namespace
    {
        // Menu code (verified in the IDB).
        constexpr std::uintptr_t input_filler_address = 0x433C8238u;    // called by the container input handler 0x4333F951
        constexpr std::uintptr_t render_filler_address = 0x433FC71Au;   // called by the container render routine 0x433D0330
        constexpr std::uintptr_t sort_address = 0x433EE548u;            // stable sort, __fastcall(begin, end, comparator)
        constexpr std::uintptr_t input_comparator_address = 0x4341F30Eu;  // open popups (vtable slot 12 > 0) first
        constexpr std::uintptr_t render_comparator_address = 0x4333C986u; // open popups last, drawn on top

        // push ebx/ebp/esi/edi, mov edi, [esp+14h]  and  push ebx/ebp/esi, mov esi, [esp+10h], push edi
        constexpr std::array<std::uint8_t, 8> input_filler_prologue{ 0x53, 0x55, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x14 };
        constexpr std::array<std::uint8_t, 8> render_filler_prologue{ 0x53, 0x55, 0x56, 0x8B, 0x74, 0x24, 0x10, 0x57 };
        constexpr std::size_t jump_size = 5;

        constexpr std::size_t container_children_begin = 0x80;
        constexpr std::size_t container_children_end = 0x84;
        constexpr std::size_t visible_flag_offset = 0x15;               // Header::Flags.Visible
        constexpr std::size_t stack_slots = 0x400;                      // the caller's list: 0x1000 bytes

        // The caller's list: begin, end, capacity, one unused dword, then the slots.
        struct ChildList
        {
            void** begin;
            void** end;
            void** capacity;
            std::uint32_t unused;
            void* slots[stack_slots];
        };
        static_assert(offsetof(ChildList, slots) == 0x10 && sizeof(ChildList) == 0x1010);

        using SortFn = void(__fastcall*)(void**, void**, std::uintptr_t);

        // tools/ui_children_harness.cpp swaps the sort for a stub to test the fillers without the game.
#ifdef SKEETSDK_UI_CHILDREN_TEST
#define SKEETSDK_GAME_FN
#else
#define SKEETSDK_GAME_FN const
#endif
        SKEETSDK_GAME_FN auto sort = reinterpret_cast<SortFn>(sort_address);

        // Buffers for lists past 1024 controls. The input handler runs on the window thread and drawing on the
        // render thread, so each filler has its own; a few per filler, used in turn, in case a callback a control
        // runs while the handler walks its list ever makes another container fill one.
        constexpr std::size_t buffers_per_filler = 4;

        struct Overflow
        {
            std::array<std::vector<void*>, buffers_per_filler> buffers;
            std::size_t next{};
        };

        Overflow input_overflow;
        Overflow render_overflow;

        struct PatchState
        {
            std::mutex mutex;
            bool installed{};
            std::array<std::uint8_t, jump_size> input_original{};
            std::array<std::uint8_t, jump_size> render_original{};
        };

        PatchState patch_state;

        template <typename T>
        T& At(void* base, std::size_t offset) noexcept
        {
            return *reinterpret_cast<T*>(static_cast<std::uint8_t*>(base) + offset);
        }

        void Fill(void* container, ChildList* out, bool visible_only, Overflow& overflow, std::uintptr_t comparator) noexcept
        {
            auto** const first = At<void**>(container, container_children_begin);
            auto** const last = At<void**>(container, container_children_end);

            std::size_t count = 0;
            for (auto** child = first; child != last; ++child)
                if (!visible_only || At<std::uint8_t>(*child, visible_flag_offset) != 0) ++count;

            void** slots = out->slots;
            std::size_t capacity = stack_slots;
            if (count > stack_slots)
            {
                auto& buffer = overflow.buffers[overflow.next];
                overflow.next = (overflow.next + 1) % buffers_per_filler;
                try
                {
                    if (buffer.size() < count) buffer.resize(count);
                    slots = buffer.data();
                    capacity = count;
                }
                catch (...)
                {
                    count = stack_slots; // what the game would have done
                }
            }

            void** end = slots;
            for (auto** child = first; child != last && static_cast<std::size_t>(end - slots) < count; ++child)
                if (!visible_only || At<std::uint8_t>(*child, visible_flag_offset) != 0) *end++ = *child;

            out->begin = slots;
            out->end = end;
            out->capacity = slots + capacity;
            sort(out->begin, out->end, comparator);
        }

        // __fastcall with the list as the only stack argument: ecx = container, retn 4, like the game's __thiscall.
        ChildList* __fastcall FillForInput(void* container, void*, ChildList* out) noexcept
        {
            Fill(container, out, false, input_overflow, input_comparator_address);
            return out;
        }

        ChildList* __fastcall FillForRender(void* container, void*, ChildList* out) noexcept
        {
            Fill(container, out, true, render_overflow, render_comparator_address);
            return out;
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

        template <std::size_t N>
        bool HasBytes(std::uintptr_t address, const std::array<std::uint8_t, N>& expected) noexcept
        {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(address);
            return IsReadable(bytes, N) && std::memcmp(bytes, expected.data(), N) == 0;
        }

        bool WriteCode(std::uintptr_t address, const std::uint8_t* bytes, std::size_t size) noexcept
        {
            auto* target = reinterpret_cast<std::uint8_t*>(address);
            DWORD old_protection{};
            if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &old_protection)) return false;
            std::memcpy(target, bytes, size);
            DWORD ignored{};
            VirtualProtect(target, size, old_protection, &ignored);
            FlushInstructionCache(GetCurrentProcess(), target, size);
            return true;
        }

        // The whole function is replaced, so a jump at its start is all it takes; nothing returns into the rest.
        bool WriteJump(std::uintptr_t address, const void* destination, std::array<std::uint8_t, jump_size>& original) noexcept
        {
            std::array<std::uint8_t, jump_size> jump{ 0xE9 };
            const auto relative = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(destination) - (address + jump_size));
            std::memcpy(jump.data() + 1, &relative, sizeof(relative));
            std::memcpy(original.data(), reinterpret_cast<const void*>(address), jump_size);
            return WriteCode(address, jump.data(), jump_size);
        }
    }

    bool Install() noexcept
    {
        std::scoped_lock lock(patch_state.mutex);
        if (patch_state.installed) return true;
        if (!HasBytes(input_filler_address, input_filler_prologue) || !HasBytes(render_filler_address, render_filler_prologue))
            return false;
        if (!WriteJump(input_filler_address, reinterpret_cast<const void*>(&FillForInput), patch_state.input_original))
            return false;
        if (!WriteJump(render_filler_address, reinterpret_cast<const void*>(&FillForRender), patch_state.render_original))
        {
            WriteCode(input_filler_address, patch_state.input_original.data(), jump_size);
            return false;
        }
        patch_state.installed = true;
        return true;
    }

    void Remove() noexcept
    {
        std::scoped_lock lock(patch_state.mutex);
        if (!patch_state.installed) return;
        WriteCode(input_filler_address, patch_state.input_original.data(), jump_size);
        WriteCode(render_filler_address, patch_state.render_original.data(), jump_size);
        patch_state.installed = false;
    }

    bool IsInstalled() noexcept
    {
        std::scoped_lock lock(patch_state.mutex);
        return patch_state.installed;
    }
#else
    bool Install() noexcept { return false; }
    void Remove() noexcept {}
    bool IsInstalled() noexcept { return false; }
#endif
}
