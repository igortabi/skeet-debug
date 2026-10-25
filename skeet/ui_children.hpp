#pragma once

namespace skeetsdk::ui_children
{
    // Containers with more than 1024 controls:
    //
    // A container hands its controls to its input handler (0x4333F951) and its render routine (0x433D0330) through
    // a list on the caller's stack with room for 1024 pointers. The routines that fill it (0x433C8238 for input,
    // every control; 0x433FC71A for drawing, the visible ones) stop at 1024, so the controls after that are never
    // clicked or never drawn. Input counts hidden controls too, which is why a script with many ui.set_visible'd
    // controls ends up with controls that draw but do not react.
    //
    // Install() replaces both fillers with ones that use the stack list up to 1024 controls, exactly as before,
    // and a buffer of their own past that. The callers only read the list's begin and end, and the game's own sort
    // still orders it (open popups first for input, last for drawing).
    //
    // It patches code inside the CRC-checked range, so call it BEFORE skeet_t::extra(), like ui_tabs::Install.
    bool Install() noexcept;
    void Remove() noexcept;
    bool IsInstalled() noexcept;
}
