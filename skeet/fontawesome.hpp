#pragma once

namespace skeetsdk::fontawesome
{
    // Font Awesome Free icons as text, so they can go into any label the menu draws:
    //
    //   skeetsdk.get_icon(name)  ->  string, or nil and a message
    //
    //   ui.new_checkbox("LUA", "A", skeetsdk.get_icon("car") .. "  Drive")
    //
    // `name` is a Font Awesome Free icon name (or alias): "car", "gear", "home". Prefix a style with a slash to
    // pick it: "solid/heart", "regular/heart", "brands/discord". Without a style the icon's main one is used
    // (solid, else brands, else regular). The result is one Private Use Area character in UTF-8; text built
    // from it works anywhere the menu draws text: checkboxes, sliders, buttons, labels, tab names.
    //
    // How it works. The cheat draws text with its own glyph cache on top of GDI: for each character it asks
    // the font's list of supported code points (GetFontUnicodeRanges), then measures and rasterizes the
    // glyph on the font's own device context (GetCharABCWidthsW / GetCharWidth32W / GetGlyphOutlineW or
    // ExtTextOutW). Install() embeds the Font Awesome fonts as private process fonts and redirects exactly
    // those calls, at the cheat's own call sites, to functions that
    //   - add the icons' code points to the font's supported list, and
    //   - for an icon's code point, run the call with the matching Font Awesome font (same height as the
    //     text, a little smaller) selected instead of the menu font, and put the menu font back afterwards.
    // Every other character, and every other user of GDI (the game itself), goes straight to gdi32.
    //
    // Regular and brands icons that share a code point with a solid one (or with each other) get an unused
    // Private Use code point as an alias; GDI chooses glyphs by code point only. get_icon hands out the alias.

    // Installs the font redirection. Like config_scripts::Install and ui_tabs::Install it patches code inside
    // the CRC-checked range, so call it AFTER skeet_t::fix_imports() (the import sites it redirects are
    // written there) and BEFORE skeet_t::extra(). Returns false, and changes nothing, if a font cannot be
    // loaded or a call site is not what it was checked against; LastError() says which.
    bool Install() noexcept;
    bool IsInstalled() noexcept;
    const char* LastError() noexcept;

    // Adds skeetsdk.get_icon to the Lua state. Meant to be called for every script load (the Lua guard does);
    // it is idempotent and cheap. Without a successful Install() get_icon returns nil and a message.
    void RegisterLuaApi(void* lua_state) noexcept;
}
