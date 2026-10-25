// Offline test for fitted listboxes in skeet/ui_listbox.cpp, on a fake listbox laid out like the real one (vtable
// at +0, container at +0x10, type at +0x19, size at +0x28, default size at +0x30, row height at +0x60, scrollbar
// at +0x68, search box at +0x7C, rows at +0x80, scroll offset at +0x84, items at +0x90) and a fake control record
// vector. The records and the listbox vtable are pointed at the fakes through the SKEETSDK_UI_LISTBOX_TEST seam.
// Nothing here touches the game.
//
// The fake scale handler does what the game's does (0x4339E4DF): size = default size at the scale, row height = 18
// at the scale, rows = height / row - search box, scrollbar on when the items outnumber the rows. Both round half
// up, which at 125% makes a plain listbox lose a row; the test shows that, and that a fitted one does not.
//
// Build and run (x86 developer prompt, from the repo root):
//   cl /nologo /std:c++latest /EHsc /MD /Od /RTC1 /Fe:%TEMP%\ui_listbox_harness.exe tools\ui_listbox_harness.cpp
//   %TEMP%\ui_listbox_harness.exe

#define SKEETSDK_UI_LISTBOX_TEST
#include "../skeet/ui_listbox.cpp"

#include <cstdio>
#include <vector>

namespace skeetsdk::ui_listbox
{
    namespace
    {
        int failures = 0;

        void Check(bool ok, const char* what)
        {
            std::printf("  [%s] %s\n", ok ? "ok  " : "FAIL", what);
            if (!ok) ++failures;
        }

        double menu_scale = 1.0;
        int layouts = 0;
        std::array<void*, listbox_vtable_slots + 1> fake_listbox_vtable{};

        int Scaled(int pixels)
        {
            return static_cast<int>(std::floor(pixels * menu_scale + 0.5));
        }

        void __fastcall FakeListboxScale(void* listbox, void*)
        {
            At<int>(listbox, control_size) = Scaled(At<int>(listbox, control_default_size));
            At<int>(listbox, control_size + 4) = Scaled(At<int>(listbox, control_default_size + 4));
            const int row = Scaled(listbox_row_pixels);
            At<int>(listbox, listbox_row_height) = row;
            const int search = At<std::uint8_t>(listbox, listbox_search) != 0 ? 1 : 0;
            const int rows = At<int>(listbox, control_size + 4) / row - search;
            At<int>(listbox, listbox_rows) = rows;
            At<std::uint8_t>(listbox, listbox_scrollbar) = static_cast<int>(ItemCount(listbox)) > rows ? 1 : 0;
        }

        void __fastcall FakeLayout(void*, void*)
        {
            ++layouts;
        }

        void __fastcall FakeNothing(void*, void*) {}

        struct FakeContainer
        {
            void* const* vtable;
            std::array<void*, 16> slots{};
            FakeContainer()
            {
                slots.fill(reinterpret_cast<void*>(&FakeNothing));
                slots[vtable_layout] = reinterpret_cast<void*>(&FakeLayout);
                vtable = slots.data();
            }
        };

        // A listbox as ui.new_listbox leaves it: 158 x 300 with a search box, scaled once, holding `count` items.
        struct FakeListbox
        {
            std::array<std::uint8_t, listbox_object_size> bytes{};
            std::vector<std::uint8_t> items;
            FakeListbox(void* container, std::size_t count, bool search = true)
            {
                At<void* const*>(bytes.data(), 0) = fake_listbox_vtable.data() + 1;
                At<void*>(bytes.data(), control_parent) = container;
                At<std::uint8_t>(bytes.data(), control_type) = type_listbox;
                At<int>(bytes.data(), control_default_size) = 158;
                At<int>(bytes.data(), control_default_size + 4) = 300;
                At<std::uint8_t>(bytes.data(), listbox_search) = search ? 1 : 0;
                SetItems(count);
                FakeListboxScale(Control(), nullptr);
            }
            void SetItems(std::size_t count)
            {
                items.assign(count * listbox_item_size, 0);
                At<std::uint8_t*>(bytes.data(), listbox_items) = items.data();
                At<std::uint8_t*>(bytes.data(), listbox_items + 4) = items.data() + items.size();
                At<std::uint8_t*>(bytes.data(), listbox_items + 8) = items.data() + items.size();
            }
            void* Control() { return bytes.data(); }
            int Height() { return At<int>(bytes.data(), control_size + 4); }
            int Row() { return At<int>(bytes.data(), listbox_row_height); }
            int Rows() { return At<int>(bytes.data(), listbox_rows); }
            bool Scrollbar() { return At<std::uint8_t>(bytes.data(), listbox_scrollbar) != 0; }
        };

        struct FakeRecords
        {
            std::vector<std::uint8_t> storage;
            std::array<std::uint8_t*, 3> vector{};
            explicit FakeRecords(std::initializer_list<void*> controls) : storage(controls.size() * control_record_size, 0)
            {
                std::size_t i = 0;
                for (void* control : controls) At<void*>(storage.data(), i++ * control_record_size) = control;
                vector = { storage.data(), storage.data() + storage.size(), storage.data() + storage.size() };
                control_records = reinterpret_cast<std::uintptr_t>(vector.data());
            }
        };

        void SetUpFakes()
        {
            fake_listbox_vtable.fill(reinterpret_cast<void*>(&FakeNothing));
            fake_listbox_vtable[1 + vtable_apply_scale] = reinterpret_cast<void*>(&FakeListboxScale);
            listbox_vtable = reinterpret_cast<std::uintptr_t>(fake_listbox_vtable.data() + 1);
        }

        // The game calls the scale handler as __thiscall, through the listbox's vtable.
        void ScaleAsGame(void* listbox)
        {
            reinterpret_cast<void(__thiscall*)(void*)>(VtableOf(listbox)[vtable_apply_scale])(listbox);
        }

        void TestFit()
        {
            std::puts("fit a listbox to its items");
            menu_scale = 1.0;
            FakeContainer container;
            FakeListbox fitted(&container, 5);
            FakeListbox plain(&container, 5);
            FakeRecords records{ fitted.Control(), plain.Control() };

            Check(ControlAt(0) == fitted.Control() && ControlAt(2) == nullptr, "references find their records");
            Check(fitted.Height() == 300 && fitted.Rows() == 15, "ui.new_listbox's listbox: 300 px, 15 rows");

            layouts = 0;
            Check(Fit(fitted.Control()) == Status::Done, "Fit succeeds");
            Check(VtableOf(fitted.Control()) == FittedVtable(), "the listbox uses the fitted vtable");
            Check(VtableOf(plain.Control()) == fake_listbox_vtable.data() + 1, "other listboxes are untouched");
            Check(FittedVtable()[vtable_apply_scale] == reinterpret_cast<void*>(&ApplyScaleFitted), "slot 10 is wrapped");
            Check(FittedVtable()[vtable_layout] == fake_listbox_vtable[1 + vtable_layout], "other slots are the listbox's own");
            Check(fitted.Height() == 6 * 18, "height: the search box and 5 items");
            Check(fitted.Rows() == 5 && !fitted.Scrollbar(), "5 rows, no scrollbar");
            Check(At<int>(fitted.Control(), control_default_size + 4) == 6 * 18, "default height matches");
            Check(layouts == 1, "the container laid out its controls again");

            std::puts("menu scale 125%");
            menu_scale = 1.25;
            At<int>(plain.Control(), control_default_size + 4) = 6 * 18;  // a plain listbox the same size
            ScaleAsGame(plain.Control());
            Check(plain.Rows() == 4 && plain.Scrollbar(), "a plain listbox of that size loses a row to rounding");
            ScaleAsGame(fitted.Control());
            Check(fitted.Row() == 23 && fitted.Height() == 6 * 23, "the fitted one is 6 rows of the new height");
            Check(fitted.Rows() == 5 && !fitted.Scrollbar(), "and still shows all 5 items");
            menu_scale = 2.0;
            ScaleAsGame(fitted.Control());
            Check(fitted.Height() == 6 * 36 && fitted.Rows() == 5, "200% too");
            menu_scale = 1.0;
            ScaleAsGame(fitted.Control());
            Check(fitted.Height() == 6 * 18, "and back to 100%");
        }

        void TestRefit()
        {
            std::puts("fit again after the items change");
            menu_scale = 1.0;
            FakeContainer container;
            FakeListbox listbox(&container, 3);
            FakeRecords records{ listbox.Control() };
            Fit(listbox.Control());

            listbox.SetItems(12);
            At<std::uint8_t>(listbox.Control(), listbox_scrollbar) = 1;   // what the item setter leaves behind
            At<int>(listbox.Control(), listbox_scroll) = 54;
            layouts = 0;
            Check(Fit(listbox.Control()) == Status::Done, "Fit on a fitted listbox succeeds");
            Check(listbox.Height() == 13 * 18 && listbox.Rows() == 12, "grows to 12 items");
            Check(!listbox.Scrollbar() && At<int>(listbox.Control(), listbox_scroll) == 0, "scrollbar off, scrolled to the top");
            Check(layouts == 1, "laid out again");

            listbox.SetItems(0);
            Fit(listbox.Control());
            Check(listbox.Height() == 2 * 18 && listbox.Rows() == 1, "an empty list keeps one row");

            FakeListbox no_search(&container, 4, false);
            FakeRecords other{ no_search.Control() };
            Fit(no_search.Control());
            Check(no_search.Height() == 4 * 18 && no_search.Rows() == 4, "without a search box: just the items");
        }

        void TestRefusals()
        {
            std::puts("refuse what is not a listbox");
            FakeContainer container;
            FakeListbox listbox(&container, 3);
            At<std::uint8_t>(listbox.Control(), control_type) = 8;   // a label's type with the listbox's vtable
            Check(Fit(listbox.Control()) == Status::NotListbox, "wrong type byte");
            At<std::uint8_t>(listbox.Control(), control_type) = type_listbox;
            std::array<void*, listbox_vtable_slots + 1> other_vtable = fake_listbox_vtable;
            At<void* const*>(listbox.Control(), 0) = other_vtable.data() + 1;
            Check(Fit(listbox.Control()) == Status::NotListbox, "wrong vtable");
            Check(listbox.Height() == 300, "left as it was");
        }
    }
}

int main()
{
    using namespace skeetsdk::ui_listbox;
    SetUpFakes();
    TestFit();
    TestRefit();
    TestRefusals();
    std::printf("%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
