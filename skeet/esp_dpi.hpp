#pragma once

namespace skeetsdk::esp_dpi
{
    // A DPI scale of its own for what the cheat draws in game (ESP and the other in-game visuals, and scripts'
    // paint callbacks), so MISC > Settings > "DPI scale" only scales the menu and paint_ui:
    //
    //   MISC > Settings > "ESP DPI scale": 100% (the default) to 200%, or "Same as menu" (the old behaviour).
    //   Saved in configs, and kept across script reloads.
    //
    // The cheat's DPI scale (0x4339F0AD) is one float at 0x4346D490, from the table at 0x434745CC. Its frame
    // (0x433AD576) records the in-game features in one loop (over 0x43475A9C), before paint_ui and the menu, and
    // hands each of them round(scale * 350), worked out just before the loop: sizes come from that and the global
    // while recording. Text gets its size from its font, picked when the recording is replayed on the render
    // thread: the drawing backend (renderer+8) keeps an unscaled set of 7 fonts (+0x108) and one built for the
    // DPI scale (+0x15C, rebuilt by 0x433C95CE when the wanted scale at +0x100 is not the built one at +0x104).
    //
    // Install() makes the loop's first instruction (0x433AD5FC) and the first one after it on every path
    // (0x433AD626) call stubs that set the global and that value to the ESP scale and put them back, and record two
    // commands around the ESP's own: type 0 with a magic in their data. Slot 0 of the replay's handler table
    // (0x4346D690) gets a wrapper that, for those two, swaps a set of fonts made at the ESP scale (with the cheat's
    // create_font, 0x4334E194) into +0x15C and sets +0x104 to the ESP scale, and swaps them back after; every other
    // type 0 command goes to the original handler. Glyphs are made while recording, in the menu's fonts, so slot 10
    // (text) gets a wrapper that adds an ESP text's glyphs to the ESP's fonts (0x43342F16) before drawing it.
    //
    // It patches code inside the CRC-checked range, so call it BEFORE skeet_t::extra(), like ui_tabs::Install.
    bool Install() noexcept;
    bool IsInstalled() noexcept;

    // Adds the "ESP DPI scale" control (and the internal skeetsdk._set_esp_dpi) the first time a script loads after
    // `ui` exists. Meant to be called for every script load (the Lua guard does); idempotent and cheap.
    void RegisterLuaApi(void* lua_state) noexcept;
}
