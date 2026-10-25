#include "crash_report.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <DbgHelp.h>
#include <intrin.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace skeetsdk::crash_report
{
    namespace
    {
        constexpr std::uintptr_t cheat_image_base = 0x43310000u;
        constexpr std::size_t stack_scan_dwords = 0x800;
        constexpr int max_stack_entries = 24;
        constexpr int max_dumps = 3;

        thread_local bool describing = false;

        bool Readable(std::uintptr_t address, std::size_t size) noexcept
        {
            while (size != 0)
            {
                MEMORY_BASIC_INFORMATION info{};
                if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == 0) return false;
                if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
                const auto end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
                if (end - address >= size) return true;
                size -= end - address;
                address = end;
            }
            return true;
        }

        bool Executable(std::uintptr_t address) noexcept
        {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == 0) return false;
            return info.State == MEM_COMMIT && (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0
                && (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        }

        std::wstring FileName(HMODULE module) noexcept
        {
            wchar_t path[MAX_PATH]{};
            if (GetModuleFileNameW(module, path, MAX_PATH) == 0) return L"?";
            const wchar_t* name = path;
            for (const wchar_t* p = path; *p != 0; ++p)
                if (*p == L'\\' || *p == L'/') name = p + 1;
            return name;
        }

        // module+offset, cheat+offset, or alloc(base)+offset for memory that is not an image.
        std::wstring Where(std::uintptr_t address) noexcept
        {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == 0) return L"?";
            if (info.State != MEM_COMMIT) return L"not mapped";
            const auto base = reinterpret_cast<std::uintptr_t>(info.AllocationBase);
            if (info.Type == MEM_IMAGE)
                return std::format(L"{}+0x{:x}", FileName(static_cast<HMODULE>(info.AllocationBase)), address - base);
            if (base == cheat_image_base) return std::format(L"cheat+0x{:x}", address - base);
            return std::format(L"alloc(0x{:x})+0x{:x}", base, address - base);
        }

        std::wstring Region(std::uintptr_t address) noexcept
        {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == 0) return L"?";
            const wchar_t* type = info.Type == MEM_IMAGE ? L"image" : info.Type == MEM_MAPPED ? L"mapped"
                : info.Type == MEM_PRIVATE ? L"private" : L"-";
            return std::format(L"alloc 0x{:x}, region 0x{:x} size 0x{:x}, state 0x{:x}, protect 0x{:x} (alloc 0x{:x}), {}",
                reinterpret_cast<std::uintptr_t>(info.AllocationBase), reinterpret_cast<std::uintptr_t>(info.BaseAddress),
                info.RegionSize, info.State, info.Protect, info.AllocationProtect, type);
        }

        std::wstring Bytes(std::uintptr_t address, std::size_t size) noexcept
        {
            if (!Readable(address, size)) return L"(unreadable)";
            std::wstring text;
            for (std::size_t i = 0; i < size; ++i)
                text += std::format(L"{:02X} ", reinterpret_cast<const std::uint8_t*>(address)[i]);
            return text;
        }
    }

    std::wstring Describe(const _EXCEPTION_POINTERS* exception) noexcept
    {
        if (describing || exception == nullptr) return {};
        describing = true;
        std::wstring text;
        try
        {
            const EXCEPTION_RECORD& record = *exception->ExceptionRecord;
            const CONTEXT& context = *exception->ContextRecord;
            const std::uintptr_t eip = context.Eip;

            if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2)
            {
                const auto kind = record.ExceptionInformation[0];
                text += std::format(L"{} 0x{:x}\n", kind == 0 ? L"read of" : kind == 1 ? L"write to" : L"execute at",
                    record.ExceptionInformation[1]);
            }
            text += std::format(L"eip {}\n  {}\n", Where(eip), Region(eip));
            text += std::format(L"  -16: {}\n  eip: {}\n", Bytes(eip - 16, 16), Bytes(eip, 16));
            text += std::format(L"eax {:08x} ebx {:08x} ecx {:08x} edx {:08x}\nesi {:08x} edi {:08x} ebp {:08x} esp {:08x}\n",
                context.Eax, context.Ebx, context.Ecx, context.Edx, context.Esi, context.Edi, context.Ebp, context.Esp);
            text += std::format(L"skeet.dll 0x{:x}, engine.dll 0x{:x}, thread {}\n",
                reinterpret_cast<std::uintptr_t>(&__ImageBase),
                reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"engine.dll")), GetCurrentThreadId());

            // Code addresses on the stack (return addresses, mostly), up to the top of this thread's stack.
            const auto stack_top = static_cast<std::uintptr_t>(__readfsdword(4));
            std::uintptr_t slot = context.Esp & ~std::uintptr_t{ 3 };
            int found = 0;
            text += L"stack:\n";
            for (std::size_t i = 0; i < stack_scan_dwords && slot + 4 <= stack_top && found < max_stack_entries; ++i, slot += 4)
            {
                if (!Readable(slot, 4)) break;
                const auto value = *reinterpret_cast<const std::uint32_t*>(slot);
                if (value < 0x10000 || !Executable(value)) continue;
                text += std::format(L"  esp+{:x}: {}\n", slot - context.Esp, Where(value));
                ++found;
            }
        }
        catch (...)
        {
        }
        describing = false;
        return text;
    }

    std::wstring WriteDump(_EXCEPTION_POINTERS* exception) noexcept
    {
        static LONG written = 0;
        if (exception == nullptr || InterlockedIncrement(&written) > max_dumps) return {};
        using WriteFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
            PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
        HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
        const auto write = dbghelp != nullptr ? reinterpret_cast<WriteFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
        if (write == nullptr) return {};

        wchar_t directory[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, directory, MAX_PATH);
        for (DWORD i = length; i > 0; --i)
            if (directory[i - 1] == L'\\')
            {
                directory[i - 1] = 0;
                break;
            }
        std::wstring path;
        try
        {
            path = std::format(L"{}\\skeet_crash_{}_{}.dmp", directory, GetCurrentProcessId(), written);
        }
        catch (...)
        {
            return {};
        }
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return {};
        MINIDUMP_EXCEPTION_INFORMATION information{ GetCurrentThreadId(), exception, FALSE };
        const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithPrivateReadWriteMemory
            | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithFullMemoryInfo | MiniDumpWithThreadInfo
            | MiniDumpWithUnloadedModules | MiniDumpWithHandleData);
        const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), file, type, &information, nullptr, nullptr);
        CloseHandle(file);
        return ok ? path : std::wstring{};
    }
}
