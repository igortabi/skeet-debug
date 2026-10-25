#pragma once

namespace skeetsdk::renderer_blur
{
    // renderer.blur:
    //
    // The cheat drew renderer.blur's rects only during the game's in-game rendering, so out of game they piled up
    // until the list held 0x2000 rects and the first frame that drew them crashed with "too many indices for index
    // buffer". In game they were drawn under everything the game draws later (loading screen, team select, buy
    // menu, console), and never in the main menu.
    //
    // Install() makes three changes:
    //   - renderer.blur drops the rect when the list already holds 4096.
    //   - Right after paint_ui, the frame's rects are taken off the cheat's list, so the game never draws them.
    //   - Right before the cheat replays its overlay at present time, they are drawn with Direct3D: the finished
    //     frame is shrunk to a quarter, blurred (a 9-tap Gaussian pixel shader, twice across and down) and drawn into
    //     each rect. So blur works in every game state, over everything the game drew and under everything the
    //     cheat draws (paint and paint_ui drawings, the menu).
    //
    // It patches code inside the CRC-checked range, so call it BEFORE skeet_t::extra(), like ui_tabs::Install.
    bool Install() noexcept;
    void Remove() noexcept;
    bool IsInstalled() noexcept;
}
