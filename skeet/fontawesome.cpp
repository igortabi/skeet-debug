#include "fontawesome.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

// See fontawesome.hpp for what this does and why it is built this way. The cheat's text engine (reverse
// engineered from the IDB) works per character on the font's own device context:
//   font creation   CreateFontIndirectW, then GetFontUnicodeRanges twice (size, data); the ranges become a
//                   bitmap of supported BMP code points in the font object, which is checked first for
//                   every character (an unsupported one is never drawn, and a failed glyph clears its bit)
//   glyph caching   GetCharABCWidthsW / GetGlyphOutlineW(GGO_GRAY8_BITMAP) for antialiased fonts,
//                   GetCharWidth32W / ExtTextOutW of one character into a DIB for the others
// Those references are instructions in the cheat's image that skeet_t::fix_imports() points at gdi32 (a
// `call rel32`, or a `mov eax, imm32` that loads the function's address); the sites are listed in
// Binary/imports.h. Install() points the ones above at the functions in this file.

namespace skeetsdk::fontawesome
{
    namespace
    {
        constexpr std::uint8_t style_solid = 0;
        constexpr std::uint8_t style_regular = 1;
        constexpr std::uint8_t style_brands = 2;

        struct Icon
        {
            std::uint16_t primary;          // real code point, in primary_style
            std::uint8_t primary_style;
            std::uint16_t regular;          // alias code point for the regular style, 0 if none
            std::uint16_t brands;           // alias code point for the brands style, 0 if none
        };

        struct Name
        {
            const char* name;
            std::uint16_t icon;             // index into kIcons
        };

#include "fontawesome_icons.inc"

        // Code points we handle live in the Private Use Area; kind_of / real_of cover all of it.
        constexpr UINT pua_first = 0xE000;
        constexpr UINT pua_count = 0x1900;
        constexpr double icon_scale = 0.9;  // icon height relative to the text it sits in

        std::array<std::uint8_t, pua_count> kind_of{};      // 0 not ours, else style + 1
        std::array<std::uint16_t, pua_count> real_of{};     // code point inside the font, 0 = same as the key
        std::vector<WCRANGE> extra_ranges;                  // what GetFontUnicodeRanges gets appended
        DWORD extra_glyphs = 0;

        std::atomic<bool> installed{ false };
        const char* last_error = "not installed";
        char error_buffer[160];

        bool Fail(const char* message) noexcept
        {
            last_error = message;
            return false;
        }

        // For "<api name> ... 0x<address>" messages.
        bool FailAt(const char* format, const char* api, unsigned address = 0) noexcept
        {
            std::snprintf(error_buffer, sizeof(error_buffer), format, api, address);
            last_error = error_buffer;
            return false;
        }

        // --- tables ---------------------------------------------------------------------------------

        bool SetKind(UINT code_point, std::uint8_t style, UINT real) noexcept
        {
            const UINT index = code_point - pua_first;
            if (index >= pua_count || kind_of[index] != 0) return false; // outside the PUA, or taken twice
            kind_of[index] = static_cast<std::uint8_t>(style + 1);
            real_of[index] = real != code_point ? static_cast<std::uint16_t>(real) : std::uint16_t{ 0 };
            return true;
        }

        bool BuildTables()
        {
            kind_of.fill(0);
            real_of.fill(0);
            for (const Icon& icon : kIcons)
            {
                if (!SetKind(icon.primary, icon.primary_style, icon.primary)) return false;
                if (icon.regular != 0 && !SetKind(icon.regular, style_regular, icon.primary)) return false;
                if (icon.brands != 0 && !SetKind(icon.brands, style_brands, icon.primary)) return false;
            }
            extra_ranges.clear();
            extra_glyphs = 0;
            for (UINT i = 0; i < pua_count;)
            {
                if (kind_of[i] == 0) { ++i; continue; }
                UINT run = i;
                while (run < pua_count && kind_of[run] != 0) ++run;
                WCRANGE range{};
                range.wcLow = static_cast<WCHAR>(pua_first + i);
                range.cGlyphs = static_cast<USHORT>(run - i);
                extra_ranges.push_back(range);
                extra_glyphs += run - i;
                i = run;
            }
            return !extra_ranges.empty();
        }

        // Which font and which code point inside it a code point stands for.
        struct Glyph
        {
            int style;      // -1: not an icon
            UINT real;
        };

        Glyph Classify(UINT ch) noexcept
        {
            if (!installed.load(std::memory_order_acquire)) return { -1, ch };
            const UINT index = ch - pua_first;
            if (index >= pua_count || kind_of[index] == 0) return { -1, ch };
            return { kind_of[index] - 1, real_of[index] != 0 ? real_of[index] : ch };
        }

        // --- fonts ----------------------------------------------------------------------------------

        const wchar_t* FaceOf(int style) noexcept
        {
            switch (style)
            {
            case style_solid: return kFaceSolid;
            case style_regular: return kFaceRegular;
            default: return kFaceBrands;
            }
        }

        struct CachedFont
        {
            LONG height;
            BYTE quality;
            BYTE style;
            HFONT font;
        };

        constexpr std::size_t max_cached_fonts = 32;
        std::array<CachedFont, max_cached_fonts> font_cache{};
        std::size_t font_cache_size = 0;
        std::mutex font_cache_mutex;

        // A Font Awesome font for the style, as big as the font selected into `dc` (a bit smaller).
        HFONT FontFor(HDC dc, int style) noexcept
        {
            const HGDIOBJ current = GetCurrentObject(dc, OBJ_FONT);
            LOGFONTW base{};
            if (current == nullptr || GetObjectW(current, sizeof(base), &base) != static_cast<int>(sizeof(base)))
                return nullptr;
            const LONG height = base.lfHeight != 0 ? base.lfHeight : -12;
            LONG target = static_cast<LONG>(std::lround(static_cast<double>(height) * icon_scale));
            if (target == 0) target = height < 0 ? -1 : 1;

            std::lock_guard lock(font_cache_mutex);
            for (std::size_t i = 0; i < font_cache_size; ++i)
            {
                const CachedFont& cached = font_cache[i];
                if (cached.height == target && cached.quality == base.lfQuality && cached.style == style)
                    return cached.font;
            }
            if (font_cache_size == font_cache.size()) return nullptr; // never seen: more sizes than anyone uses

            LOGFONTW wanted{};
            wanted.lfHeight = target;
            wanted.lfWeight = FW_NORMAL;        // each style is its own face; solid is heavy by itself
            wanted.lfCharSet = DEFAULT_CHARSET;
            wanted.lfOutPrecision = OUT_DEFAULT_PRECIS;
            wanted.lfClipPrecision = CLIP_DEFAULT_PRECIS;
            wanted.lfQuality = base.lfQuality;
            wanted.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
            const wchar_t* face = FaceOf(style);
            std::size_t length = 0;
            while (face[length] != L'\0' && length + 1 < LF_FACESIZE) { wanted.lfFaceName[length] = face[length]; ++length; }
            const HFONT font = CreateFontIndirectW(&wanted);
            if (font == nullptr) return nullptr;
            font_cache[font_cache_size++] = { target, base.lfQuality, static_cast<BYTE>(style), font };
            return font;
        }

        // Selects the icon font into the device context for the length of a call, then puts the old one back.
        class FontScope
        {
        public:
            FontScope(HDC dc, int style) noexcept : dc_(dc)
            {
                if (const HFONT font = FontFor(dc, style)) previous_ = SelectObject(dc, font);
            }
            ~FontScope()
            {
                if (previous_ != nullptr) SelectObject(dc_, previous_);
            }
            FontScope(const FontScope&) = delete;
            FontScope& operator=(const FontScope&) = delete;
            explicit operator bool() const noexcept { return previous_ != nullptr; }

        private:
            HDC dc_;
            HGDIOBJ previous_ = nullptr;
        };

        // --- the redirected gdi32 calls -----------------------------------------------------------

        // Advance and bearings from the glyph metrics, for fonts GDI will not give ABC widths for.
        bool GlyphMetricsOf(HDC dc, UINT code_point, GLYPHMETRICS& metrics) noexcept
        {
            const MAT2 identity{ { 0, 1 }, { 0, 0 }, { 0, 0 }, { 0, 1 } };
            return GetGlyphOutlineW(dc, code_point, GGO_METRICS, &metrics, 0, nullptr, &identity) != GDI_ERROR;
        }

        BOOL WINAPI HookGetCharABCWidthsW(HDC dc, UINT first, UINT last, LPABC abc)
        {
            if (first == last)
            {
                const Glyph glyph = Classify(first);
                if (glyph.style >= 0)
                {
                    const FontScope scope(dc, glyph.style);
                    if (scope)
                    {
                        if (GetCharABCWidthsW(dc, glyph.real, glyph.real, abc)) return TRUE;
                        GLYPHMETRICS metrics{};
                        if (!GlyphMetricsOf(dc, glyph.real, metrics)) return FALSE;
                        abc->abcA = metrics.gmptGlyphOrigin.x;
                        abc->abcB = metrics.gmBlackBoxX;
                        abc->abcC = metrics.gmCellIncX - abc->abcA - static_cast<int>(abc->abcB);
                        return TRUE;
                    }
                }
            }
            return GetCharABCWidthsW(dc, first, last, abc);
        }

        BOOL WINAPI HookGetCharWidth32W(HDC dc, UINT first, UINT last, LPINT widths)
        {
            if (first == last)
            {
                const Glyph glyph = Classify(first);
                if (glyph.style >= 0)
                {
                    const FontScope scope(dc, glyph.style);
                    if (scope)
                    {
                        if (GetCharWidth32W(dc, glyph.real, glyph.real, widths)) return TRUE;
                        GLYPHMETRICS metrics{};
                        if (!GlyphMetricsOf(dc, glyph.real, metrics)) return FALSE;
                        *widths = metrics.gmCellIncX;
                        return TRUE;
                    }
                }
            }
            return GetCharWidth32W(dc, first, last, widths);
        }

        DWORD WINAPI HookGetGlyphOutlineW(HDC dc, UINT ch, UINT format, LPGLYPHMETRICS metrics, DWORD size,
            LPVOID buffer, const MAT2* matrix)
        {
            if ((format & GGO_GLYPH_INDEX) == 0)    // with the flag `ch` is a glyph index, not a character
            {
                const Glyph glyph = Classify(ch);
                if (glyph.style >= 0)
                {
                    const FontScope scope(dc, glyph.style);
                    if (scope) return GetGlyphOutlineW(dc, glyph.real, format, metrics, size, buffer, matrix);
                }
            }
            return GetGlyphOutlineW(dc, ch, format, metrics, size, buffer, matrix);
        }

        BOOL WINAPI HookExtTextOutW(HDC dc, int x, int y, UINT options, const RECT* rect, LPCWSTR text, UINT count,
            const INT* dx)
        {
            if (text != nullptr && count == 1)      // the engine draws one character at a time
            {
                const Glyph glyph = Classify(text[0]);
                if (glyph.style >= 0)
                {
                    TEXTMETRICW base{};
                    const bool have_base = GetTextMetricsW(dc, &base) != FALSE;
                    const FontScope scope(dc, glyph.style);
                    if (scope)
                    {
                        // Drawn from the top, the icon would sit where the menu font's top is: line the
                        // baselines up instead.
                        TEXTMETRICW icon{};
                        if (have_base && (GetTextAlign(dc) & TA_BASELINE) == 0 && GetTextMetricsW(dc, &icon))
                            y += base.tmAscent - icon.tmAscent;
                        const wchar_t real = static_cast<wchar_t>(glyph.real);
                        return ExtTextOutW(dc, x, y, options, rect, &real, 1, dx);
                    }
                }
            }
            return ExtTextOutW(dc, x, y, options, rect, text, count, dx);
        }

        // The size query (lpgs == nullptr) and the fill must agree: the caller sizes its buffer from the first.
        DWORD WINAPI HookGetFontUnicodeRanges(HDC dc, LPGLYPHSET set)
        {
            const DWORD original = GetFontUnicodeRanges(dc, set);
            if (original == 0 || !installed.load(std::memory_order_acquire)) return original;
            const DWORD extra = static_cast<DWORD>(extra_ranges.size() * sizeof(WCRANGE));
            if (set == nullptr) return original + extra;
            WCRANGE* out = set->ranges + set->cRanges;
            std::memcpy(out, extra_ranges.data(), extra);
            set->cRanges += static_cast<DWORD>(extra_ranges.size());
            set->cGlyphsSupported += extra_glyphs;
            set->cbThis += extra;
            return original + extra;
        }

        // --- icon names -----------------------------------------------------------------------------

        const Name* FindName(const char* name) noexcept
        {
            std::size_t low = 0;
            std::size_t high = std::size(kNames);
            while (low < high)
            {
                const std::size_t mid = low + (high - low) / 2;
                const int order = std::strcmp(kNames[mid].name, name);
                if (order == 0) return &kNames[mid];
                if (order < 0) low = mid + 1; else high = mid;
            }
            return nullptr;
        }

        const char* StyleName(int style) noexcept
        {
            return style == style_solid ? "solid" : style == style_regular ? "regular" : "brands";
        }

        // "car", "solid/car", "regular/heart", "brands/discord"; "fa-car" and "fas/far/fab" work too.
        // Returns the code point to put in text (0 and an error message when there is none).
        UINT ResolveIcon(std::string spec, std::string& error)
        {
            for (char& c : spec) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            int wanted = -1;
            const std::size_t slash = spec.find('/');
            if (slash != std::string::npos)
            {
                const std::string style = spec.substr(0, slash);
                spec.erase(0, slash + 1);
                if (style == "solid" || style == "fas") wanted = style_solid;
                else if (style == "regular" || style == "far") wanted = style_regular;
                else if (style == "brands" || style == "fab") wanted = style_brands;
                else { error = "unknown style '" + style + "' (use solid, regular or brands)"; return 0; }
            }
            if (spec.compare(0, 3, "fa-") == 0) spec.erase(0, 3);
            const Name* found = FindName(spec.c_str());
            if (found == nullptr) { error = "unknown icon '" + spec + "'"; return 0; }
            const Icon& icon = kIcons[found->icon];
            if (wanted < 0 || wanted == icon.primary_style) return icon.primary;
            const UINT alias = wanted == style_regular ? icon.regular : wanted == style_brands ? icon.brands : 0;
            if (alias != 0) return alias;
            error = "icon '" + spec + "' has no " + StyleName(wanted) + " style (it has " + StyleName(icon.primary_style);
            if (icon.regular != 0) error += ", regular";
            if (icon.brands != 0) error += ", brands";
            error += ")";
            return 0;
        }

        std::string Utf8(UINT code_point)
        {
            std::string out;
            out += static_cast<char>(0xE0 | (code_point >> 12));
            out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code_point & 0x3F));
            return out;
        }

        // --- installing -----------------------------------------------------------------------------

        HMODULE ThisModule() noexcept
        {
            HMODULE module = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&ThisModule), &module);
            return module;
        }

        bool AddFont(HMODULE module, const wchar_t* resource) noexcept
        {
            const HRSRC found = FindResourceW(module, resource, MAKEINTRESOURCEW(10)); // RT_RCDATA
            if (found == nullptr) return false;
            const HGLOBAL loaded = LoadResource(module, found);
            const DWORD size = SizeofResource(module, found);
            const void* data = loaded != nullptr ? LockResource(loaded) : nullptr;
            if (data == nullptr || size == 0) return false;
            DWORD fonts = 0;
            // A private font: only this process sees it, and it goes away with it. The data stays mapped.
            return AddFontMemResourceEx(const_cast<void*>(data), size, nullptr, &fonts) != nullptr && fonts > 0;
        }

        // The face really loaded (CreateFontIndirectW quietly substitutes another one when it did not) and has
        // the glyph we will ask it for.
        bool FaceWorks(int style, UINT sample) noexcept
        {
            const HDC dc = CreateCompatibleDC(nullptr);
            if (dc == nullptr) return false;
            LOGFONTW wanted{};
            wanted.lfHeight = -16;
            wanted.lfWeight = FW_NORMAL;
            wanted.lfCharSet = DEFAULT_CHARSET;
            const wchar_t* face = FaceOf(style);
            for (std::size_t i = 0; face[i] != L'\0' && i + 1 < LF_FACESIZE; ++i) wanted.lfFaceName[i] = face[i];
            const HFONT font = CreateFontIndirectW(&wanted);
            bool works = false;
            if (font != nullptr)
            {
                const HGDIOBJ previous = SelectObject(dc, font);
                wchar_t actual[LF_FACESIZE]{};
                GetTextFaceW(dc, LF_FACESIZE, actual);
                WORD index = 0xFFFF;
                const wchar_t character = static_cast<wchar_t>(sample);
                works = _wcsicmp(actual, face) == 0
                    && GetGlyphIndicesW(dc, &character, 1, &index, GGI_MARK_NONEXISTING_GLYPHS) == 1 && index != 0xFFFF;
                SelectObject(dc, previous);
                DeleteObject(font);
            }
            DeleteDC(dc);
            return works;
        }

        // Tables and fonts: everything Install() does except touching the cheat's code.
        bool PrepareFonts() noexcept
        {
            try
            {
                if (!BuildTables()) return Fail("the icon table is not consistent");
                const HMODULE module = ThisModule();
                if (module == nullptr) return Fail("cannot find this module");
                if (!AddFont(module, L"FA_SOLID")) return Fail("cannot load the Font Awesome solid font");
                if (!AddFont(module, L"FA_REGULAR")) return Fail("cannot load the Font Awesome regular font");
                if (!AddFont(module, L"FA_BRANDS")) return Fail("cannot load the Font Awesome brands font");
                std::string ignored;
                const UINT car = ResolveIcon("solid/car", ignored);
                const UINT heart = ResolveIcon("solid/heart", ignored);
                const UINT discord = ResolveIcon("brands/discord", ignored);
                if (car == 0 || heart == 0 || discord == 0) return Fail("the icon table has no car, heart or discord");
                if (!FaceWorks(style_solid, car)) return Fail("Windows did not accept the Font Awesome solid font");
                if (!FaceWorks(style_regular, heart)) return Fail("Windows did not accept the Font Awesome regular font");
                if (!FaceWorks(style_brands, discord)) return Fail("Windows did not accept the Font Awesome brands font");
                return true;
            }
            catch (...)
            {
                return Fail("out of memory");
            }
        }

#if defined(_M_IX86)
        bool IsReadable(const void* address, std::size_t size) noexcept
        {
            MEMORY_BASIC_INFORMATION memory{};
            if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory)) return false;
            if (memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
            const auto begin = reinterpret_cast<std::uintptr_t>(address);
            const auto end = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
            return begin + size <= end;
        }

        // Call sites in the cheat's image that skeet_t::fix_imports() points at gdi32 (Binary/imports.h).
        struct Site
        {
            std::uintptr_t address;
            const char* api;
            const void* hook;
        };

        const Site sites[] = {
            { 0x4334E29B, "GetFontUnicodeRanges", reinterpret_cast<const void*>(&HookGetFontUnicodeRanges) },
            { 0x4334E2C0, "GetFontUnicodeRanges", reinterpret_cast<const void*>(&HookGetFontUnicodeRanges) },
            { 0x4332F2B9, "GetCharABCWidthsW", reinterpret_cast<const void*>(&HookGetCharABCWidthsW) },
            { 0x433CA6C4, "GetCharABCWidthsW", reinterpret_cast<const void*>(&HookGetCharABCWidthsW) },
            { 0x4332F331, "GetGlyphOutlineW", reinterpret_cast<const void*>(&HookGetGlyphOutlineW) },
            { 0x4332F36F, "GetGlyphOutlineW", reinterpret_cast<const void*>(&HookGetGlyphOutlineW) },
            { 0x433CA6EE, "GetGlyphOutlineW", reinterpret_cast<const void*>(&HookGetGlyphOutlineW) },
            { 0x4332F5E9, "GetCharWidth32W", reinterpret_cast<const void*>(&HookGetCharWidth32W) },
            { 0x433CA6D6, "GetCharWidth32W", reinterpret_cast<const void*>(&HookGetCharWidth32W) },
            { 0x4332F70F, "ExtTextOutW", reinterpret_cast<const void*>(&HookExtTextOutW) },
            { 0x433CAB7A, "ExtTextOutW", reinterpret_cast<const void*>(&HookExtTextOutW) },
        };

        // fix_imports() writes a site in one of two forms: a relative E8/E9 call, or an opcode followed by the
        // function's absolute address (the cheat loads some pointers with `mov eax, imm32`). Reads where the
        // site points now; false for any other instruction.
        bool ReadSite(std::uintptr_t site, bool& relative, std::uintptr_t& target) noexcept
        {
            const auto* code = reinterpret_cast<const std::uint8_t*>(site);
            if (!IsReadable(code, 5)) return false;
            std::int32_t value = 0;
            std::memcpy(&value, code + 1, sizeof(value));
            if (code[0] == 0xE8 || code[0] == 0xE9)
            {
                relative = true;
                target = site + 5 + static_cast<std::uintptr_t>(value);
                return true;
            }
            if ((code[0] >= 0xB8 && code[0] <= 0xBF) || code[0] == 0x68)    // mov r32, imm32 / push imm32
            {
                relative = false;
                target = static_cast<std::uintptr_t>(static_cast<std::uint32_t>(value));
                return true;
            }
            return false;
        }

        // Makes the site point at `hook` instead, keeping its form (relative call or absolute address).
        bool Redirect(std::uintptr_t site, std::uintptr_t hook) noexcept
        {
            bool relative = false;
            std::uintptr_t target = 0;
            if (!ReadSite(site, relative, target)) return false;
            auto* code = reinterpret_cast<std::uint8_t*>(site);
            DWORD old_protection{};
            if (!VirtualProtect(code, 5, PAGE_EXECUTE_READWRITE, &old_protection)) return false;
            const auto operand = relative ? static_cast<std::uint32_t>(hook - site - 5)
                                          : static_cast<std::uint32_t>(hook);
            std::memcpy(code + 1, &operand, sizeof(operand));
            DWORD ignored{};
            VirtualProtect(code, 5, old_protection, &ignored);
            FlushInstructionCache(GetCurrentProcess(), code, 5);
            return true;
        }

        // All or nothing: every site must point at the expected gdi32 function before any is changed.
        bool PatchCallSites() noexcept
        {
            const HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
            if (gdi == nullptr) return Fail("gdi32.dll is not loaded");
            for (const Site& site : sites)
            {
                const auto expected = reinterpret_cast<std::uintptr_t>(GetProcAddress(gdi, site.api));
                if (expected == 0) return FailAt("gdi32 has no %s", site.api);
                bool relative = false;
                std::uintptr_t target = 0;
                if (!ReadSite(site.address, relative, target) || target != expected)
                    return FailAt("the reference to %s at 0x%08X is not where it should be", site.api,
                        static_cast<unsigned>(site.address));
            }
            for (const Site& site : sites)
            {
                if (!Redirect(site.address, reinterpret_cast<std::uintptr_t>(site.hook)))
                    return FailAt("cannot patch the reference to %s at 0x%08X", site.api, static_cast<unsigned>(site.address));
            }
            return true;
        }

        // --- Lua (same C API addresses as ui_tabs.cpp) ------------------------------------------

        constexpr std::uintptr_t lua_push_cclosure_address = 0x43432F73u;
        constexpr std::uintptr_t lua_set_field_address = 0x43435B37u;
        constexpr std::uintptr_t lua_push_string_address = 0x434370E4u;
        constexpr std::uintptr_t lua_set_top_address = 0x43426BDFu;
        constexpr std::uintptr_t lua_find_table_address = 0x43426632u;
        constexpr std::uintptr_t lua_type_address = 0x4343654Au;
        constexpr std::uintptr_t lua_to_lstring_address = 0x43435597u;
        constexpr std::uintptr_t lua_get_field_address = 0x4342EBA2u;
        constexpr int lua_registry_index = -10000;
        constexpr int lua_type_string = 4;
        constexpr int lua_type_function = 6;

        using LuaCFunction = int(__cdecl*)(void*);
        using PushCClosureFn = void(__cdecl*)(void*, LuaCFunction, int);
        using SetFieldFn = void(__cdecl*)(void*, int, const char*);
        using PushStringFn = void(__cdecl*)(void*, const char*);
        using SetTopFn = void(__cdecl*)(void*, int);
        using FindTableFn = const char*(__cdecl*)(void*, int, const char*, int);
        using TypeFn = int(__cdecl*)(void*, int);
        using ToLStringFn = const char*(__cdecl*)(void*, int, std::size_t*);
        using GetFieldFn = void(__cdecl*)(void*, int, const char*);

        const auto push_cclosure = reinterpret_cast<PushCClosureFn>(lua_push_cclosure_address);
        const auto set_field = reinterpret_cast<SetFieldFn>(lua_set_field_address);
        const auto push_string = reinterpret_cast<PushStringFn>(lua_push_string_address);
        const auto set_top = reinterpret_cast<SetTopFn>(lua_set_top_address);
        const auto find_table = reinterpret_cast<FindTableFn>(lua_find_table_address);
        const auto get_type = reinterpret_cast<TypeFn>(lua_type_address);
        const auto to_lstring = reinterpret_cast<ToLStringFn>(lua_to_lstring_address);
        const auto get_field = reinterpret_cast<GetFieldFn>(lua_get_field_address);

        int FailLua(void* lua_state, const char* message) noexcept
        {
            push_string(lua_state, nullptr);
            push_string(lua_state, message);
            return 2;
        }

        int __cdecl NativeGetIcon(void* lua_state) noexcept
        {
            try
            {
                if (get_type(lua_state, 1) != lua_type_string)
                    return FailLua(lua_state, "bad argument #1 to 'get_icon' (icon name expected)");
                std::size_t length = 0;
                const char* text = to_lstring(lua_state, 1, &length);
                if (text == nullptr || length == 0 || length > 64)
                    return FailLua(lua_state, "bad argument #1 to 'get_icon' (icon name expected)");
                if (!installed.load(std::memory_order_acquire))
                {
                    const std::string message = std::string("Font Awesome is not available: ") + last_error;
                    return FailLua(lua_state, message.c_str());
                }
                std::string error;
                const UINT code_point = ResolveIcon(std::string(text, length), error);
                if (code_point == 0) return FailLua(lua_state, error.c_str());
                push_string(lua_state, Utf8(code_point).c_str());
                return 1;
            }
            catch (...)
            {
                return FailLua(lua_state, "out of memory");
            }
        }
#endif
    }

    bool Install() noexcept
    {
#if defined(_M_IX86)
        if (installed.load(std::memory_order_acquire)) return true;
        if (!PrepareFonts()) return false;
        if (!PatchCallSites()) return false;
        installed.store(true, std::memory_order_release);
        last_error = "";
        return true;
#else
        return Fail("only the 32-bit build has Font Awesome support");
#endif
    }

    bool IsInstalled() noexcept
    {
        return installed.load(std::memory_order_acquire);
    }

    const char* LastError() noexcept
    {
        return last_error;
    }

    void RegisterLuaApi(void* lua_state) noexcept
    {
#if defined(_M_IX86)
        if (find_table(lua_state, lua_registry_index, "_LOADED._G.skeetsdk", 4) != nullptr) return;
        // Runs on every load and the GC is off, so do not allocate the closure again every time.
        get_field(lua_state, -1, "get_icon");
        const bool registered = get_type(lua_state, -1) == lua_type_function;
        set_top(lua_state, -2);
        if (!registered)
        {
            push_cclosure(lua_state, &NativeGetIcon, 0);
            set_field(lua_state, -2, "get_icon");
        }
        set_top(lua_state, -2);
#else
        (void)lua_state;
#endif
    }
}
