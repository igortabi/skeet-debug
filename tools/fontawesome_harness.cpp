// Offline test for skeet/fontawesome.cpp: runs the GDI calls the cheat's glyph cache makes (see the comment
// at the top of fontawesome.cpp) against the redirect functions, on a memory device context with an ordinary
// text font selected, and prints what comes out. Nothing here touches the game or the cheat's code.
//
// Build and run (x86 developer prompt, from the repo root):
//   rc /nologo /fo %TEMP%\fa.res skeet\fontawesome.rc
//   cl /nologo /std:c++latest /EHsc /MD /Fe:%TEMP%\fa_harness.exe tools\fontawesome_harness.cpp %TEMP%\fa.res gdi32.lib user32.lib
//   %TEMP%\fa_harness.exe

#include "../skeet/fontawesome.cpp"

#include <cstdio>
#include <string>

namespace skeetsdk::fontawesome
{
    namespace
    {
        int failures = 0;

        void Check(bool ok, const char* what)
        {
            std::printf("  [%s] %s\n", ok ? "ok  " : "FAIL", what);
            if (!ok) ++failures;
        }

        HFONT MakeFont(const wchar_t* face, int height, BYTE quality)
        {
            LOGFONTW lf{};
            lf.lfHeight = height;
            lf.lfWeight = FW_NORMAL;
            lf.lfCharSet = DEFAULT_CHARSET;
            lf.lfQuality = quality;
            for (std::size_t i = 0; face[i] != L'\0' && i + 1 < LF_FACESIZE; ++i) lf.lfFaceName[i] = face[i];
            return CreateFontIndirectW(&lf);
        }

        void PrintGlyph(HDC dc, UINT ch, const char* title)
        {
            const MAT2 identity{ { 0, 1 }, { 0, 0 }, { 0, 0 }, { 0, 1 } };
            GLYPHMETRICS gm{};
            const DWORD size = HookGetGlyphOutlineW(dc, ch, GGO_GRAY8_BITMAP, &gm, 0, nullptr, &identity);
            std::printf("  %s U+%04X: bitmap %u bytes, black box %ux%u, origin (%d,%d), advance %d\n", title, ch,
                size, gm.gmBlackBoxX, gm.gmBlackBoxY, gm.gmptGlyphOrigin.x, gm.gmptGlyphOrigin.y, gm.gmCellIncX);
            if (size == 0 || size == GDI_ERROR) return;
            std::vector<unsigned char> bits(size);
            HookGetGlyphOutlineW(dc, ch, GGO_GRAY8_BITMAP, &gm, size, bits.data(), &identity);
            const unsigned pitch = (gm.gmBlackBoxX + 3) & ~3u;
            static const char shade[] = " .:-=+*#%@";
            for (unsigned y = 0; y < gm.gmBlackBoxY; ++y)
            {
                std::printf("    ");
                for (unsigned x = 0; x < gm.gmBlackBoxX; ++x)
                {
                    const unsigned v = bits[y * pitch + x];
                    std::putchar(shade[std::min(9u, v * 10 / 65)]);
                }
                std::putchar('\n');
            }
        }

        // Draws one character the way the engine's non-antialiased path does and reports the ink bounds.
        struct Ink { int left, top, right, bottom; int pixels; };
        Ink DrawChar(HDC base_dc, HFONT font, wchar_t ch, bool through_hook, int x, int y)
        {
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(info.bmiHeader);
            info.bmiHeader.biWidth = 64;
            info.bmiHeader.biHeight = -64;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            void* pixels = nullptr;
            const HBITMAP dib = CreateDIBSection(base_dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
            const HDC dc = CreateCompatibleDC(base_dc);
            const HGDIOBJ old_bitmap = SelectObject(dc, dib);
            const HGDIOBJ old_font = SelectObject(dc, font);
            SetBkColor(dc, 0);
            SetBkMode(dc, OPAQUE);
            SetTextColor(dc, 0xFFFFFF);
            SetTextAlign(dc, 0);
            std::memset(pixels, 0, 64 * 64 * 4);
            if (through_hook) HookExtTextOutW(dc, x, y, 0, nullptr, &ch, 1, nullptr);
            else ExtTextOutW(dc, x, y, 0, nullptr, &ch, 1, nullptr);
            GdiFlush();
            Ink ink{ 64, 64, -1, -1, 0 };
            const auto* p = static_cast<const std::uint32_t*>(pixels);
            for (int row = 0; row < 64; ++row)
                for (int col = 0; col < 64; ++col)
                    if (p[row * 64 + col] & 0xFF00)
                    {
                        ink.left = std::min(ink.left, col); ink.right = std::max(ink.right, col);
                        ink.top = std::min(ink.top, row); ink.bottom = std::max(ink.bottom, row);
                        ++ink.pixels;
                    }
            SelectObject(dc, old_font);
            SelectObject(dc, old_bitmap);
            DeleteDC(dc);
            DeleteObject(dib);
            return ink;
        }

        int WINAPI MarkerA() { return 41; }
        int WINAPI MarkerB() { return 42; }

        // The two forms fix_imports() writes, built in executable memory: `call rel32` and `mov eax, imm32; call eax`.
        void TestSites()
        {
            std::printf("call sites\n");
            auto* page = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            auto* relative_stub = page;          // E8 rel32 ; C3
            auto* absolute_stub = page + 32;     // B8 imm32 ; FF D0 ; C3
            const auto a = reinterpret_cast<std::uintptr_t>(&MarkerA);
            const auto b = reinterpret_cast<std::uintptr_t>(&MarkerB);
            const auto rel = static_cast<std::uint32_t>(a - reinterpret_cast<std::uintptr_t>(relative_stub) - 5);
            relative_stub[0] = 0xE8; std::memcpy(relative_stub + 1, &rel, 4); relative_stub[5] = 0xC3;
            const auto abs_a = static_cast<std::uint32_t>(a);
            absolute_stub[0] = 0xB8; std::memcpy(absolute_stub + 1, &abs_a, 4); absolute_stub[5] = 0xFF; absolute_stub[6] = 0xD0; absolute_stub[7] = 0xC3;
            using Stub = int(__stdcall*)();
            Check(reinterpret_cast<Stub>(relative_stub)() == 41 && reinterpret_cast<Stub>(absolute_stub)() == 41, "both stubs reach A");
            bool relative = false;
            std::uintptr_t target = 0;
            Check(ReadSite(reinterpret_cast<std::uintptr_t>(relative_stub), relative, target) && relative && target == a, "reads the relative form");
            Check(ReadSite(reinterpret_cast<std::uintptr_t>(absolute_stub), relative, target) && !relative && target == a, "reads the absolute form");
            Check(Redirect(reinterpret_cast<std::uintptr_t>(relative_stub), b) && Redirect(reinterpret_cast<std::uintptr_t>(absolute_stub), b),
                "redirects both");
            Check(reinterpret_cast<Stub>(relative_stub)() == 42 && reinterpret_cast<Stub>(absolute_stub)() == 42, "both stubs now reach B");
            page[64] = 0x90;                     // not a reference at all
            Check(!ReadSite(reinterpret_cast<std::uintptr_t>(page + 64), relative, target) && !Redirect(reinterpret_cast<std::uintptr_t>(page + 64), b),
                "any other instruction is refused");
            VirtualFree(page, 0, MEM_RELEASE);
        }

        void TestNames()
        {
            std::printf("names\n");
            std::string error;
            const UINT car = ResolveIcon("car", error);
            Check(car == 0xF1B9, "car -> U+F1B9");
            Check(ResolveIcon("automobile", error) == car, "alias automobile -> car");
            Check(ResolveIcon("FA-Car", error) == car, "case and fa- prefix are ignored");
            Check(ResolveIcon("solid/car", error) == car, "solid/car");
            Check(ResolveIcon("home", error) == ResolveIcon("house", error), "alias home -> house");
            const UINT heart = ResolveIcon("heart", error);
            const UINT regular_heart = ResolveIcon("regular/heart", error);
            Check(heart == 0xF004, "heart -> U+F004 (solid)");
            Check(regular_heart != 0 && regular_heart != heart, "regular/heart is a different code point");
            Check(ResolveIcon("brands/discord", error) == 0xF392 && ResolveIcon("discord", error) == 0xF392,
                "discord is brands without a prefix");
            Check(ResolveIcon("regular/car", error) == 0 && !error.empty(), "regular/car is refused");
            std::printf("    message: %s\n", error.c_str());
            Check(ResolveIcon("nosuchicon", error) == 0, "unknown icon is refused");
            std::printf("    message: %s\n", error.c_str());
            Check(ResolveIcon("weird/car", error) == 0, "unknown style is refused");
            Check(ResolveIcon("a", error) == 0, "ASCII-named icons are not in the table");
            Check(Classify('A').style < 0 && Classify(0xE000 - 1).style < 0 && Classify(0xF8FF + 1).style < 0,
                "ordinary text is never classified as an icon");
            Check(Classify(car).style == style_solid && Classify(regular_heart).style == style_regular
                && Classify(regular_heart).real == heart && Classify(0xF392).style == style_brands,
                "styles and alias targets");
        }

        void TestRanges(HDC dc)
        {
            std::printf("GetFontUnicodeRanges\n");
            const DWORD original = GetFontUnicodeRanges(dc, nullptr);
            const DWORD size = HookGetFontUnicodeRanges(dc, nullptr);
            Check(size > original && size == original + extra_ranges.size() * sizeof(WCRANGE), "size query adds our ranges");
            std::vector<unsigned char> buffer(size + 16, 0xCD);
            auto* set = reinterpret_cast<GLYPHSET*>(buffer.data());
            const DWORD filled = HookGetFontUnicodeRanges(dc, set);
            Check(filled == size && set->cbThis == size, "fill agrees with the size query");
            Check(buffer[size] == 0xCD && buffer[size + 15] == 0xCD, "nothing written past the promised size");
            // the engine's helper: set a bit per code point of every range
            std::vector<bool> supported(0x10000, false);
            for (DWORD i = 0; i < set->cRanges; ++i)
                for (unsigned c = set->ranges[i].wcLow; c < unsigned(set->ranges[i].wcLow) + set->ranges[i].cGlyphs; ++c)
                    if (c < 0x10000) supported[c] = true;
            Check(supported['A'], "the font's own ranges are still there ('A')");
            Check(supported[0xF1B9] && supported[0xF004] && supported[0xF392], "car, heart and discord are supported");
            std::string error;
            Check(supported[ResolveIcon("regular/heart", error)], "the regular alias is supported");
        }

        void TestMetrics(HDC dc, HFONT base)
        {
            std::printf("widths and glyphs\n");
            std::string error;
            const UINT codes[] = { ResolveIcon("car", error), ResolveIcon("regular/heart", error),
                ResolveIcon("brands/discord", error), ResolveIcon("gear", error) };
            ABC text_abc{};
            Check(HookGetCharABCWidthsW(dc, 'A', 'A', &text_abc) && text_abc.abcB > 0, "'A' passes through untouched");
            for (const UINT code : codes)
            {
                ABC abc{};
                int width = 0;
                const BOOL abc_ok = HookGetCharABCWidthsW(dc, code, code, &abc);
                const BOOL width_ok = HookGetCharWidth32W(dc, code, code, &width);
                char line[160];
                std::snprintf(line, sizeof(line), "U+%04X ABC (%d,%u,%d) width %d", code, abc.abcA, abc.abcB, abc.abcC, width);
                Check(abc_ok && width_ok && abc.abcB > 0 && width > 0 && width < 40, line);
                Check(GetCurrentObject(dc, OBJ_FONT) == base, "the menu font is selected again afterwards");
            }
            PrintGlyph(dc, codes[0], "car (solid)");
            PrintGlyph(dc, codes[1], "heart (regular alias)");
            PrintGlyph(dc, codes[2], "discord (brands)");
        }

        void TestDraw(HDC dc, HFONT base_font, const char* label)
        {
            std::printf("ExtTextOutW, non-antialiased path (%s)\n", label);
            std::string error;
            const wchar_t car = static_cast<wchar_t>(ResolveIcon("car", error));
            const Ink a = DrawChar(dc, base_font, L'H', false, 8, 8);
            const Ink icon = DrawChar(dc, base_font, car, true, 8, 8);
            const Ink untouched = DrawChar(dc, base_font, L'H', true, 8, 8);
            std::printf("  'H' ink rows %d..%d (%d px), car ink rows %d..%d cols %d..%d (%d px)\n", a.top, a.bottom,
                a.pixels, icon.top, icon.bottom, icon.left, icon.right, icon.pixels);
            Check(icon.pixels > 20, "the icon draws something");
            Check(untouched.pixels == a.pixels && untouched.top == a.top, "ordinary text is untouched by the hook");
            // baselines line up: the icon's bottom edge is near the text's baseline (bottom of 'H'), not far off
            Check(std::abs(icon.bottom - a.bottom) <= 3, "the icon sits on the text's baseline (within 3 px)");
            Check(icon.bottom - icon.top <= (a.bottom - a.top) + 6, "the icon is about as tall as the text");
        }
    }

    int RunTests()
    {
        std::printf("Font Awesome %s: %zu icons, %zu names\n", kVersion, std::size(kIcons), std::size(kNames));
        if (!PrepareFonts())
        {
            std::printf("PrepareFonts failed: %s\n", last_error);
            return 2;
        }
        installed.store(true);
        TestSites();
        TestNames();

        const HDC screen = GetDC(nullptr);
        const HDC dc = CreateCompatibleDC(screen);
        struct Case { const wchar_t* face; int height; BYTE quality; const char* label; };
        const Case cases[] = {
            { L"Segoe UI", -13, ANTIALIASED_QUALITY, "Segoe UI 13 antialiased" },
            { L"Verdana", -11, NONANTIALIASED_QUALITY, "Verdana 11 non-antialiased" },
            { L"Tahoma", -18, CLEARTYPE_NATURAL_QUALITY, "Tahoma 18 ClearType" },
        };
        for (const Case& c : cases)
        {
            std::printf("\n== menu font: %s\n", c.label);
            const HFONT font = MakeFont(c.face, c.height, c.quality);
            const HGDIOBJ previous = SelectObject(dc, font);
            TestRanges(dc);
            TestMetrics(dc, font);
            TestDraw(dc, font, c.label);
            SelectObject(dc, previous);
            DeleteObject(font);
        }
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        std::printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL OK" : "FAILED", failures, failures == 1 ? "" : "s");
        return failures == 0 ? 0 : 1;
    }
}

int main()
{
    return skeetsdk::fontawesome::RunTests();
}
