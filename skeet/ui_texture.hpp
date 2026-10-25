#pragma once

namespace skeetsdk::ui_texture
{
    // Images in the menu, as controls of their own:
    //
    //   ui.new_texture(tab, container, texture_id, width, height)  ->  reference
    //       Adds a control that draws the texture (an id from renderer.load_png / load_jpg / load_svg /
    //       load_rgba) stretched to width x height, below the previous control and lined up with the other
    //       controls' text. width and height are pixels at 100% menu scale (whole numbers, 1 to 4096); like every
    //       control it grows with the menu's DPI scale. Raises a Lua error on failure, like ui.new_label.
    //
    //   ui.set(reference, texture_id [, width, height])
    //       Draws another texture from now on, and resizes the control when width and height are given.
    //       Any other reference goes to the game's ui.set unchanged.
    //
    //   skeetsdk.new_texture(tab, container, texture_id, width, height)  ->  reference, or nil and a message
    //   skeetsdk.set_texture(reference, texture_id [, width, height])     ->  true, or nil and a message
    //       The same two without raising.
    //
    // The control is a label made by the game's own ui.new_label with the name "\n" (which the game stores as an
    // empty name), so its reference is an ordinary one: ui.set_visible hides it, the container scrolls and clips
    // it, and the game deletes it with the script. Only its vtable is swapped for a copy whose Render draws the
    // texture and whose destructor forgets it, and its default size becomes width x height, so the label's own
    // scale handler sizes it at any menu scale. Nothing in the cheat's code is patched.
    //
    // Adds the functions above (and the internal skeetsdk._make_texture) to the Lua global `skeetsdk`, and wraps
    // ui.new_texture / ui.set in Lua the first time a script loads after `ui` exists. Meant to be called for every
    // script load (the Lua guard does); it is idempotent and cheap.
    void RegisterLuaApi(void* lua_state) noexcept;
}
