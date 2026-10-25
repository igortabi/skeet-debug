// Offline test for the container child lists in skeet/ui_children.cpp, on a fake container (children vector at
// +0x80/+0x84) whose children are fake controls (visible flag at +0x15), and a stack-sized list laid out like the
// caller's (begin, end, capacity, unused, 1024 slots). The game's sort is swapped for a stub through the
// SKEETSDK_UI_CHILDREN_TEST seam. Nothing here touches the game.
//
// Build and run (x86 developer prompt, from the repo root):
//   cl /nologo /std:c++latest /EHsc /MD /Od /RTC1 /Fe:%TEMP%\ui_children_harness.exe tools\ui_children_harness.cpp
//   %TEMP%\ui_children_harness.exe

#define SKEETSDK_UI_CHILDREN_TEST
#include "../skeet/ui_children.cpp"

#include <cstdio>
#include <memory>

namespace skeetsdk::ui_children
{
    namespace
    {
        int failures = 0;

        void Check(bool ok, const char* what)
        {
            std::printf("  [%s] %s\n", ok ? "ok  " : "FAIL", what);
            if (!ok) ++failures;
        }

        struct FakeControl
        {
            std::uint8_t bytes[0x60]{};
        };

        struct FakeContainer
        {
            std::vector<std::unique_ptr<FakeControl>> controls;
            std::vector<void*> children;
            std::uint8_t bytes[0xC8]{};

            FakeContainer(std::size_t count, std::size_t hidden_every)
            {
                for (std::size_t i = 0; i < count; ++i)
                {
                    controls.push_back(std::make_unique<FakeControl>());
                    controls.back()->bytes[visible_flag_offset] = hidden_every != 0 && i % hidden_every == 0 ? 0 : 1;
                    children.push_back(controls.back().get());
                }
                At<void**>(bytes, container_children_begin) = children.data();
                At<void**>(bytes, container_children_end) = children.data() + children.size();
            }
        };

        std::uintptr_t sorted_with = 0;
        std::size_t sorted_count = 0;

        void __fastcall SortStub(void** begin, void** end, std::uintptr_t comparator)
        {
            sorted_with = comparator;
            sorted_count = static_cast<std::size_t>(end - begin);
        }

        std::size_t Visible(const FakeContainer& container)
        {
            std::size_t count = 0;
            for (auto* child : container.children) count += static_cast<FakeControl*>(child)->bytes[visible_flag_offset];
            return count;
        }

        // Every entry of the list is the next qualifying child, in order.
        bool InOrder(const FakeContainer& container, const ChildList& list, bool visible_only)
        {
            void** entry = list.begin;
            for (auto* child : container.children)
            {
                if (visible_only && !static_cast<FakeControl*>(child)->bytes[visible_flag_offset]) continue;
                if (entry == list.end || *entry != child) return false;
                ++entry;
            }
            return entry == list.end;
        }

        void Run(std::size_t count, std::size_t hidden_every)
        {
            std::printf("%zu controls, every %zuth hidden:\n", count, hidden_every);
            FakeContainer container(count, hidden_every);
            auto list = std::make_unique<ChildList>();

            FillForInput(container.bytes, nullptr, list.get());
            const auto input_count = static_cast<std::size_t>(list->end - list->begin);
            Check(input_count == count, "input lists every control, hidden ones too");
            Check(InOrder(container, *list, false), "input keeps the children's order");
            Check((list->begin == list->slots) == (count <= stack_slots), "input uses the stack list exactly up to 1024");
            Check(list->end <= list->capacity, "input end is within capacity");
            Check(sorted_with == input_comparator_address && sorted_count == input_count, "input sorted with its comparator");

            FillForRender(container.bytes, nullptr, list.get());
            const auto render_count = static_cast<std::size_t>(list->end - list->begin);
            Check(render_count == Visible(container), "render lists the visible controls");
            Check(InOrder(container, *list, true), "render keeps the children's order");
            Check((list->begin == list->slots) == (render_count <= stack_slots), "render uses the stack list exactly up to 1024");
            Check(sorted_with == render_comparator_address && sorted_count == render_count, "render sorted with its comparator");
        }
    }
}

int main()
{
    using namespace skeetsdk::ui_children;
    sort = &SortStub;
    Run(0, 0);
    Run(10, 3);
    Run(1024, 0);
    Run(1025, 0);
    Run(1500, 2);      // input past 1024, render below: the case that used to lose clicks
    Run(5000, 0);
    Run(5000, 0);      // overflow buffers reused
    std::printf(failures == 0 ? "all passed\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
