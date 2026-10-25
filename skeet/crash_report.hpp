#pragma once

#include <string>

struct _EXCEPTION_POINTERS;

namespace skeetsdk::crash_report
{
    // What the access-violation box shows below its first line, for finding crashes in code that is not in any
    // module (JIT code, trampolines, allocations): the fault, the memory region EIP is in and who allocated it,
    // the registers, the bytes around EIP and the code addresses on the stack as module+offset.
    // Reads memory only after VirtualQuery says it is readable, and returns an empty string when it is entered
    // again from inside itself.
    std::wstring Describe(const _EXCEPTION_POINTERS* exception) noexcept;
}
