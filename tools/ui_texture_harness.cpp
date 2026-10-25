// Offline test for the texture control in skeet/ui_texture.cpp, on a fake label laid out like the real one (vtable
// at +0, container at +0x10, editable byte at +0x14, type at +0x19, position at +0x20, size at +0x28, default size
// at +0x30), a fake control record vector and a fake renderer. The control records and the label vtable are
// pointed at the fakes through the SKEETSDK_UI_TEXTURE_TEST seam. Nothing here touches the game.
//
// The renderer's texture slot is a naked stub that records ecx, edx, xmm0 and the eight stack arguments and pops
// them itself, the way the game's one does (0x43368EBB, ret 20h), so the call is checked at the register level.
// Built without optimization and with /RTC1, a slot whose calling convention does not match the caller's also
// trips the runtime's ESP check.
//
// Build and run (x86 developer prompt, from the repo root):
//   cl /nologo /std:c++latest /EHsc /MD /Od /RTC1 /Fe:%TEMP%\ui_texture_harness.exe tools\ui_texture_harness.cpp
//   %TEMP%\ui_texture_harness.exe

#define SKEETSDK_UI_TEXTURE_TEST
#include "../skeet/ui_texture.cpp"

#include <cstdio>

// What the fake renderer's texture slot was called with. Global so the naked stub can address it.
void* draw_renderer = nullptr;
int draw_texture = 0;
float draw_scale = 0.0f;
int draw_arguments[8]{};
int draw_calls = 0;

__declspec(naked) void FakeDrawTexture()
{
    __asm
    {
        mov draw_renderer, ecx
        mov draw_texture, edx
        movss draw_scale, xmm0
        push esi
        push edi
        lea esi, [esp + 12]
        lea edi, draw_arguments
        mov ecx, 8
        rep movsd
        pop edi
        pop esi
        inc draw_calls
        ret 20h
    }
}

namespace skeetsdk::ui_texture
{
    namespace
    {
        int failures = 0;

        void Check(bool ok, const char* what)
        {
            std::printf("  [%s] %s\n", ok ? "ok  " : "FAIL", what);
            if (!ok) ++failures;
        }

        // The label's slots the texture control keeps or wraps. The destructor puts the label's vtable back
        // like the real one (0x4335CA74) and records the flags; the scale handler scales the default size by
        // 125%; the container's layout counts its calls.
        std::array<void*, label_vtable_slots + 1> fake_label_vtable{};
        int label_destroyed_with = -1;
        int label_renders = 0;
        int layouts = 0;
        void* const fake_rtti = &label_renders;

        void* __fastcall FakeLabelDestroy(void* control, void*, int flags)
        {
            *static_cast<void**>(control) = fake_label_vtable.data() + 1;
            label_destroyed_with = flags;
            return control;
        }

        void __fastcall FakeLabelRender(void*, void*, void*, const int*)
        {
            ++label_renders;
        }

        void __fastcall FakeApplyScale(void* control, void*)
        {
            At<int>(control, control_size) = At<int>(control, label_default_size) * 5 / 4;
            At<int>(control, control_size + 4) = At<int>(control, label_default_size + 4) * 5 / 4;
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

        struct FakeRenderer
        {
            void* const* vtable;
            std::array<void*, 48> slots{};
            FakeRenderer()
            {
                slots[renderer_texture_slot] = reinterpret_cast<void*>(&FakeDrawTexture);
                vtable = slots.data();
            }
        };

        struct alignas(8) FakeLabel
        {
            std::array<std::uint8_t, label_object_size> bytes{};
            FakeLabel(void* container, int x, int y)
            {
                At<void* const*>(bytes.data(), 0) = fake_label_vtable.data() + 1;
                At<void*>(bytes.data(), control_parent) = container;
                At<std::uint8_t>(bytes.data(), control_editable) = 1;
                At<std::uint8_t>(bytes.data(), control_type) = type_label;
                At<int>(bytes.data(), control_pos) = x;
                At<int>(bytes.data(), control_pos + 4) = y;
                At<int>(bytes.data(), control_size) = 200;
                At<int>(bytes.data(), control_size + 4) = 12;
                At<int>(bytes.data(), label_default_size) = 200;
                At<int>(bytes.data(), label_default_size + 4) = 12;
            }
            void* Control() { return bytes.data(); }
        };

        // {first, last, end} of the control records, each 0x1C bytes with the control first.
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
            fake_label_vtable.fill(reinterpret_cast<void*>(&FakeNothing));
            fake_label_vtable[0] = fake_rtti;
            fake_label_vtable[1 + vtable_destroy] = reinterpret_cast<void*>(&FakeLabelDestroy);
            fake_label_vtable[1 + vtable_render] = reinterpret_cast<void*>(&FakeLabelRender);
            fake_label_vtable[1 + vtable_apply_scale] = reinterpret_cast<void*>(&FakeApplyScale);
            label_vtable = reinterpret_cast<std::uintptr_t>(fake_label_vtable.data() + 1);
        }

        // The game calls both slots as __thiscall: arguments on the stack, popped by the callee.
        using RenderSlot = void(__thiscall*)(void*, void*, const int*);
        using DestroySlot = void*(__thiscall*)(void*, int);

        void RenderAsGame(void* control, FakeRenderer& renderer, const int* origin)
        {
            reinterpret_cast<RenderSlot>(VtableOf(control)[vtable_render])(control, &renderer, origin);
        }

        void* DestroyAsGame(void* control, int flags)
        {
            return reinterpret_cast<DestroySlot>(VtableOf(control)[vtable_destroy])(control, flags);
        }

        bool Registered(void* control)
        {
            return FindTexture(control) != texture_controls.end();
        }

        void TestMakeAndRender()
        {
            std::puts("make a label into a texture control and draw it");
            FakeContainer container;
            FakeLabel label(&container, 20, 30);
            FakeLabel other(&container, 20, 60);
            FakeRecords records{ label.Control(), nullptr, other.Control() };
            FakeRenderer renderer;

            Check(ControlAt(0) == label.Control(), "reference 0 finds the first record's control");
            Check(ControlAt(1) == nullptr, "a record without a control gives nothing");
            Check(ControlAt(3) == nullptr, "a reference past the last record gives nothing");

            layouts = 0;
            Check(MakeTexture(label.Control(), 7, 64, 32) == Status::Done, "MakeTexture on a label succeeds");
            Check(VtableOf(label.Control()) == TextureVtable(), "the label now uses the texture vtable");
            Check(texture_vtable_storage[0] == fake_rtti, "the copy keeps the label's vtable[-1]");
            Check(TextureVtable()[vtable_apply_scale] == fake_label_vtable[1 + vtable_apply_scale], "other slots are the label's own");
            Check(At<int>(label.Control(), label_default_size) == 64 && At<int>(label.Control(), label_default_size + 4) == 32,
                "the default size is the texture's");
            Check(At<int>(label.Control(), control_size) == 80 && At<int>(label.Control(), control_size + 4) == 40,
                "the label's scale handler sized it (125%)");
            Check(layouts == 1, "the container laid out its controls again");
            Check(VtableOf(other.Control()) == fake_label_vtable.data() + 1, "other labels are untouched");

            draw_calls = 0;
            label_renders = 0;
            const int origin[2] = { 100, 200 };
            RenderAsGame(label.Control(), renderer, origin);
            Check(draw_calls == 1 && label_renders == 0, "Render draws the texture, not the label's name");
            Check(draw_renderer == &renderer, "ecx is the renderer");
            Check(draw_texture == 7, "edx is the texture id");
            Check(draw_scale == 1.0f, "xmm0 is the scale 1.0");
            Check(draw_arguments[0] == 120 && draw_arguments[1] == 230, "x, y are origin + position");
            Check(static_cast<std::uint32_t>(draw_arguments[2]) == color_enabled, "an enabled control draws opaque white");
            Check(draw_arguments[3] == 0 && draw_arguments[4] == 0, "source offset 0, 0");
            Check(draw_arguments[5] == 80 && draw_arguments[6] == 40, "width, height are the scaled size");
            Check(draw_arguments[7] == texture_mode_fill, "mode is fill");

            At<std::uint8_t>(label.Control(), control_editable) = 0;
            RenderAsGame(label.Control(), renderer, origin);
            Check(static_cast<std::uint32_t>(draw_arguments[2]) == color_disabled, "a disabled control draws with alpha 70");
            At<std::uint8_t>(label.Control(), control_editable) = 1;

            Check(MakeTexture(label.Control(), 8, 64, 32) == Status::NotLabel, "a texture control is not made twice");
            Check(MakeTexture(other.Control(), 8, 64, 32) == Status::Done, "a second label becomes a texture too");
            Check(texture_controls.size() == 2, "two controls are registered");

            Check(DestroyAsGame(other.Control(), 1) == other.Control(), "the destructor returns the control");
            Check(label_destroyed_with == 1, "the label's own destructor ran with the game's flags");
            Check(!Registered(other.Control()) && Registered(label.Control()), "only the destroyed control is forgotten");
            DestroyAsGame(label.Control(), 1);
            Check(texture_controls.empty(), "nothing is left registered");
        }

        void TestSetTexture()
        {
            std::puts("change the texture and the size");
            FakeContainer container;
            FakeLabel label(&container, 20, 30);
            FakeLabel plain(&container, 20, 60);
            FakeRecords records{ label.Control(), plain.Control() };
            FakeRenderer renderer;

            Check(SetTexture(label.Control(), 3, std::nullopt) == Status::NotTexture, "SetTexture refuses a plain label");
            Check(MakeTexture(label.Control(), 3, 48, 48) == Status::Done, "made");

            layouts = 0;
            Check(SetTexture(label.Control(), 9, std::nullopt) == Status::Done, "SetTexture without a size");
            Check(layouts == 0 && At<int>(label.Control(), label_default_size) == 48, "keeps the size and the layout");
            const int origin[2] = { 0, 0 };
            RenderAsGame(label.Control(), renderer, origin);
            Check(draw_texture == 9, "draws the new texture");

            Check(SetTexture(label.Control(), 9, std::array<int, 2>{ 100, 20 }) == Status::Done, "SetTexture with a size");
            Check(At<int>(label.Control(), control_size) == 125 && At<int>(label.Control(), control_size + 4) == 25, "resized at 125%");
            Check(layouts == 1, "and laid out again");
            Check(SetTexture(plain.Control(), 1, std::nullopt) == Status::NotTexture, "a plain label stays refused");

            DestroyAsGame(label.Control(), 1);
            Check(SetTexture(label.Control(), 1, std::nullopt) == Status::NotTexture, "a destroyed control is refused");
        }

        void TestRefusals()
        {
            std::puts("refuse what is not a label");
            FakeContainer container;
            FakeLabel label(&container, 0, 0);
            At<std::uint8_t>(label.Control(), control_type) = 3; // a checkbox's type with the label's vtable
            Check(MakeTexture(label.Control(), 1, 10, 10) == Status::NotLabel, "wrong type byte");
            At<std::uint8_t>(label.Control(), control_type) = type_label;
            std::array<void*, label_vtable_slots + 1> other_vtable = fake_label_vtable;
            At<void* const*>(label.Control(), 0) = other_vtable.data() + 1;
            Check(MakeTexture(label.Control(), 1, 10, 10) == Status::NotLabel, "wrong vtable");
            Check(texture_controls.empty(), "nothing registered");
        }
    }
}

int main()
{
    using namespace skeetsdk::ui_texture;
    SetUpFakes();
    TestMakeAndRender();
    TestSetTexture();
    TestRefusals();
    std::printf("%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
