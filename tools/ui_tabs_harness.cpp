// Offline test for the tab bar logic in skeet/ui_tabs.cpp: placing, hiding, showing and removing tabs, on a
// fake menu laid out like the real one (Tabs vector at +0x54, each tab's visible byte at +0x15, icon position
// at +0x20, size at +0x28). The two game functions the logic calls (set_visible, tab_switch) are replaced by
// stubs through the SKEETSDK_UI_TABS_TEST seam. Nothing here touches the game.
//
// Build and run (x86 developer prompt, from the repo root):
//   cl /nologo /std:c++latest /EHsc /MD /Fe:%TEMP%\ui_tabs_harness.exe tools\ui_tabs_harness.cpp user32.lib
//   %TEMP%\ui_tabs_harness.exe

#define SKEETSDK_UI_TABS_TEST
#include "../skeet/ui_tabs.cpp"

#include <cstdio>

namespace skeetsdk::ui_tabs
{
    namespace
    {
        int failures = 0;
        int switched_to = -1;

        void Check(bool ok, const char* what)
        {
            std::printf("  [%s] %s\n", ok ? "ok  " : "FAIL", what);
            if (!ok) ++failures;
        }

        // The game's set_visible without the parent notification; tab_switch just records where it was sent.
        void __fastcall StubSetVisible(void* element, void*, char visible)
        {
            At<std::uint8_t>(element, visible_flag_offset) = static_cast<std::uint8_t>(visible);
        }

        void __fastcall StubTabSwitch(void* menu, void*, unsigned index)
        {
            switched_to = static_cast<int>(index);
            At<std::uint32_t>(menu, menu_current_tab) = index;
        }

        struct FakeMenu
        {
            std::vector<std::uint8_t> menu = std::vector<std::uint8_t>(0x400, 0);
            std::array<void*, 16> slots{};
            std::vector<std::vector<std::uint8_t>> tabs;

            explicit FakeMenu(int count, int step)
            {
                tabs.reserve(32);
                for (int i = 0; i < count; ++i) Add(20 + i * step, step);
                At<std::uint8_t*>(menu.data(), menu_tabs_begin) = reinterpret_cast<std::uint8_t*>(slots.data());
                Sync();
                At<std::uint8_t*>(menu.data(), menu_tabs_end + 4) = reinterpret_cast<std::uint8_t*>(slots.data() + slots.size());
            }

            void* Menu() { return menu.data(); }
            void* Tab(std::size_t i) { return slots[i]; }
            std::size_t Count() { return static_cast<std::size_t>(At<std::uint8_t*>(menu.data(), menu_tabs_end) - At<std::uint8_t*>(menu.data(), menu_tabs_begin)) / sizeof(void*); }

            void* Add(int y, int step)
            {
                tabs.emplace_back(0x98, 0);
                void* tab = tabs.back().data();
                At<int>(tab, tab_pos) = 6;
                At<int>(tab, tab_pos + 4) = y;
                At<int>(tab, tab_size) = 75;
                At<int>(tab, tab_size + 4) = step;
                At<std::uint8_t>(tab, visible_flag_offset) = 1;
                slots[used_] = tab;
                At<int>(tab, tab_index_offset) = static_cast<int>(used_);
                ++used_;
                if (At<std::uint8_t*>(menu.data(), menu_tabs_begin) != nullptr) Sync();
                return tab;
            }

            void Sync()
            {
                At<std::uint8_t*>(menu.data(), menu_tabs_end) = reinterpret_cast<std::uint8_t*>(slots.data() + used_);
            }

            std::size_t used_ = 0;

            // y of every tab in vector order
            std::vector<int> Ys() { std::vector<int> ys; for (std::size_t i = 0; i < Count(); ++i) ys.push_back(IconY(slots[i])); return ys; }
            // y of the visible ones, sorted: a tight bar is 20, 20+step, 20+2*step, ...
            std::vector<int> BarYs()
            {
                std::vector<int> ys;
                ForEachBarTab(menu.data(), [&](void* t) { ys.push_back(IconY(t)); });
                std::sort(ys.begin(), ys.end());
                return ys;
            }
        };

        bool Tight(const std::vector<int>& sorted, int step)
        {
            for (std::size_t i = 0; i < sorted.size(); ++i)
                if (sorted[i] != 20 + static_cast<int>(i) * step) return false;
            return true;
        }

        void Run(int step)
        {
            std::printf("\n== bar step %d\n", step);
            hidden_tabs.clear();
            hidden_containers.clear();
            FakeMenu m(9, step);
            void* menu = m.Menu();
            const std::vector<int> original = m.Ys();
            Check(Tight(m.BarYs(), step), "9 built-in tabs, tight bar");

            // hide one in the middle
            Check(HideTab(menu, m.Tab(3)), "hide tab 3");
            auto ys = m.Ys();
            Check(IconY(m.Tab(3)) == hidden_icon_y && IsHiddenTab(m.Tab(3)), "its icon waits above the bar");
            Check(ys[2] == original[2] && ys[4] == original[4] - step && ys[8] == original[8] - step, "tabs below moved up one step, tabs above stayed");
            Check(Tight(m.BarYs(), step) && m.BarYs().size() == 8, "the bar is tight with 8 tabs");
            Check(At<std::uint8_t>(m.Tab(3), visible_flag_offset) == 0, "the visible byte went to 0 (the game's own way)");
            Check(hidden_tabs.size() == 1 && hidden_tabs[0].rank == 3, "rank 3 remembered");
            Check(ShowTab(menu, m.Tab(3)) && m.Ys() == original, "showing it puts everything back exactly");
            Check(At<std::uint8_t>(m.Tab(3), visible_flag_offset) == 1 && hidden_tabs.empty(), "visible byte restored, nothing left hidden");

            // hide twice is harmless, show of a visible tab is harmless
            HideTab(menu, m.Tab(1)); HideTab(menu, m.Tab(1));
            Check(hidden_tabs.size() == 1 && Tight(m.BarYs(), step), "hiding twice changes nothing more");
            ShowTab(menu, m.Tab(1)); ShowTab(menu, m.Tab(1));
            Check(m.Ys() == original, "showing twice changes nothing more");

            // first and last
            HideTab(menu, m.Tab(0));
            Check(Tight(m.BarYs(), step) && m.BarYs().size() == 8, "hide the first tab: bar still tight");
            ShowTab(menu, m.Tab(0));
            Check(m.Ys() == original, "show it: back on top");
            HideTab(menu, m.Tab(8));
            ShowTab(menu, m.Tab(8));
            Check(m.Ys() == original, "hide and show the last tab");

            // several, shown in reverse (what RestoreHidden does) and in the same order
            HideTab(menu, m.Tab(2)); HideTab(menu, m.Tab(5)); HideTab(menu, m.Tab(6));
            Check(Tight(m.BarYs(), step) && m.BarYs().size() == 6, "three hidden: 6 tabs, tight");
            ShowTab(menu, m.Tab(6)); ShowTab(menu, m.Tab(5)); ShowTab(menu, m.Tab(2));
            Check(m.Ys() == original, "shown in reverse: exactly as before");
            HideTab(menu, m.Tab(2)); HideTab(menu, m.Tab(5)); HideTab(menu, m.Tab(6));
            ShowTab(menu, m.Tab(2)); ShowTab(menu, m.Tab(5)); ShowTab(menu, m.Tab(6));
            Check(Tight(m.BarYs(), step) && m.BarYs().size() == 9, "shown in the same order: still a tight, full bar");

            // placing while one is hidden: the hidden tab holds no slot (a fresh bar, the shows above re-ordered it)
            hidden_tabs.clear();
            FakeMenu p(9, step);
            void* pm = p.Menu();
            const std::vector<int> fresh = p.Ys();
            HideTab(pm, p.Tab(4));
            const int bottom = MakeRoomInTabBar(pm, std::nullopt, step);
            Check(bottom == 20 + 8 * step, "a new tab at the bottom goes below the 8 visible ones");
            const int top = MakeRoomInTabBar(pm, 1u, step);
            Check(top == 20 && p.BarYs()[0] == 20 + step, "a new tab at 1 takes the top slot, the 8 visible ones moved down");
            ForEachBarTab(pm, [&](void* t) { IconY(t) -= step; });   // undo it: the fake has no new tab to take the slot
            ShowTab(pm, p.Tab(4));
            Check(p.Ys() == fresh, "after undoing that and showing the tab: the original bar");
        }

        void RunRemoval(int step)
        {
            std::printf("\n== removal and teardown, step %d\n", step);
            hidden_tabs.clear();
            hidden_containers.clear();
            switched_to = -1;
            FakeMenu m(9, step);
            void* menu = m.Menu();
            // two "script tabs"
            void* a = m.Add(20 + 9 * step, step);
            void* b = m.Add(20 + 10 * step, step);
            At<std::uint32_t>(menu, menu_current_tab) = 0;

            HideTab(menu, a);
            Check(Tight(m.BarYs(), step) && m.BarYs().size() == 10, "a script tab hidden: bar has 10");
            RemoveTab(menu, a);
            Check(m.Count() == 10 && !IsHiddenTab(a), "removing a hidden tab drops its hidden entry");
            Check(Tight(m.BarYs(), step) && m.BarYs().size() == 10 && IconY(b) == 20 + 9 * step, "and nothing else moved: the next tab is where it was");
            RemoveTab(menu, b);
            Check(m.Count() == 9 && Tight(m.BarYs(), step), "back to the 9 built-in tabs");

            // teardown: a built-in tab and a container hidden by a script, plus a script tab hidden too
            FakeMenu n(9, step);
            void* nm = n.Menu();
            void* script_tab = n.Add(20 + 9 * step, step);
            std::vector<std::uint8_t> container(0xC8, 0);
            At<std::uint8_t>(container.data(), visible_flag_offset) = 1;
            const std::vector<int> before = n.Ys();
            HideTab(nm, n.Tab(2));
            HideTab(nm, script_tab);
            SetContainerVisible(container.data(), false);
            Check(At<std::uint8_t>(container.data(), visible_flag_offset) == 0 && hidden_containers.size() == 1, "container hidden");
            Check(Tight(n.BarYs(), step) && n.BarYs().size() == 8, "bar: 8 visible");
            RestoreHidden(nm, std::vector<void*>{ script_tab });
            Check(At<std::uint8_t>(container.data(), visible_flag_offset) == 1 && hidden_containers.empty(), "the container is shown again");
            Check(At<std::uint8_t>(n.Tab(2), visible_flag_offset) == 1 && !IsHiddenTab(n.Tab(2)), "the hidden built-in tab is back, flag restored");
            Check(hidden_tabs.size() == 1 && hidden_tabs[0].tab == script_tab, "only the script tab is still listed: RemoveTab needs that");
            RemoveTab(nm, script_tab);
            Check(hidden_tabs.empty(), "and removing it clears the list");
            std::vector<int> after = n.Ys();
            Check(n.Count() == 9 && after == std::vector<int>(before.begin(), before.begin() + 9), "the bar is exactly as it was before the script");

            // hiding the open tab switches to the LUA tab first
            hidden_tabs.clear();
            At<std::uint32_t>(nm, menu_current_tab) = 3;
            switched_to = -1;
            HideTab(nm, n.Tab(3));
            Check(switched_to == static_cast<int>(lua_tab_index), "hiding the open tab goes to the LUA tab first");
            Check(TabAt(nm, lua_tab_index) == n.Tab(8), "TabAt finds the LUA tab (index 8)");
            ShowTab(nm, n.Tab(3));
        }
    }

    int RunTests()
    {
        set_visible = reinterpret_cast<SetVisibleFn>(&StubSetVisible);
        tab_switch = reinterpret_cast<TabSwitchFn>(&StubTabSwitch);
        Run(64);
        Run(80);
        RunRemoval(64);
        RunRemoval(80);
        std::printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL OK" : "FAILED", failures, failures == 1 ? "" : "s");
        return failures == 0 ? 0 : 1;
    }
}

int main()
{
    return skeetsdk::ui_tabs::RunTests();
}
