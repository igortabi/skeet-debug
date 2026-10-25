#include "renderer_blur.hpp"
#include "lua_loadstring_guard.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d9.h>
#include <d3dcommon.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

// The cheat's own blur (all verified in skeet.bin):
//
// renderer.blur (0x433F3D5E) appends the rect to a list of 8-byte {x, y, w, h} int16 rects at +0x28 of the blur
// context (pointer at 0x43475A20). A listener at +0x08 of the context (vtable 0x434722B8, slot 0 0x433C8896) draws
// the list with the material system during the game's in-game rendering (0x433B1183: copy the screen, blur it with
// the scope blur materials, one quad per rect) and empties it. So it only ran in game, under everything the game
// draws after the world (loading screen, team select, buy menu, console), and out of game the list just grew.
//
// The cheat's frame (0x433AD576) records its overlay every frame, in game and out: in-game paint, paint_ui (0x4336F85E,
// called at 0x433AD637), then the menu. The recording is replayed with Direct3D at present time (0x4338ACAE, called
// at 0x433841B3 from the Steam overlay's present hook), on top of everything the game drew.
//
// So here the rects are taken off the list right after paint_ui, and drawn with Direct3D right before the overlay is
// replayed: blurred from the finished frame, under everything the cheat draws, in every game state.

extern std::uint8_t* PatternScan(void* module, const char* signature);

namespace skeetsdk::renderer_blur
{
#if defined(_M_IX86)
    namespace
    {
        constexpr std::uintptr_t queue_blur_address = 0x433F2AE2u;     // __thiscall(context, x, y, w, h), retn 10h
        constexpr std::uintptr_t blur_call_address = 0x433F3DB6u;      // renderer.blur: call queue_blur
        constexpr std::uintptr_t paint_ui_address = 0x4336F85Eu;       // __fastcall(IRenderer*, int*), fires paint_ui
        constexpr std::uintptr_t paint_ui_call_address = 0x433AD637u;  // the cheat's frame: call paint_ui
        constexpr std::uintptr_t replay_overlay_address = 0x4338ACAEu; // no arguments, draws the recorded overlay
        constexpr std::uintptr_t replay_overlay_call_address = 0x433841B3u;
        constexpr std::uintptr_t blur_context_address = 0x43475A20u;

        // test ecx, ecx / jz +9 / push eax, ebp, ebx, edi / call queue_blur
        constexpr std::array<std::uint8_t, 13> blur_call_site{
            0x85, 0xC9, 0x74, 0x09, 0x50, 0x55, 0x53, 0x57, 0xE8, 0x27, 0xED, 0xFF, 0xFF };
        // lea edx, [esp+8] / mov ecx, edi / call paint_ui
        constexpr std::array<std::uint8_t, 11> paint_ui_call_site{
            0x8D, 0x54, 0x24, 0x08, 0x8B, 0xCF, 0xE8, 0x22, 0x22, 0xFC, 0xFF };
        // cmp dword ptr [0x4347C9FC], 0 / jnz +5 / call replay_overlay
        constexpr std::array<std::uint8_t, 14> replay_overlay_call_site{
            0x83, 0x3D, 0xFC, 0xC9, 0x47, 0x43, 0x00, 0x75, 0x05, 0xE8, 0xF6, 0x6A, 0x00, 0x00 };
        constexpr std::size_t blur_call_offset = 8;
        constexpr std::size_t paint_ui_call_offset = 6;
        constexpr std::size_t replay_overlay_call_offset = 9;
        constexpr std::size_t call_size = 5;

        constexpr std::size_t context_rects_begin = 0x28;
        constexpr std::size_t context_rects_end = 0x2C;

        // More than any script needs in a frame; also keeps a stuck list small.
        constexpr std::size_t max_queued = 4096;

        // shaderapidx9: mov eax, [g_pD3DDevice] / push eax / mov ecx, [eax] / call [ecx+0Ch]
        constexpr const char* device_signature = "A1 ? ? ? ? 50 8B 08 FF 51 0C";

        constexpr int downscale = 4;
        constexpr int blur_iterations = 2;

        // 9 taps, linear sampling between texels; c0.xy is the step of one tap, across or down.
        constexpr const char* blur_shader_source = R"(
sampler source : register(s0);
float4 step_size : register(c0);
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float4 color = tex2D(source, uv) * 0.2270270;
    color += (tex2D(source, uv + step_size.xy * 1.3846154) + tex2D(source, uv - step_size.xy * 1.3846154)) * 0.3162162;
    color += (tex2D(source, uv + step_size.xy * 3.2307692) + tex2D(source, uv - step_size.xy * 3.2307692)) * 0.0702703;
    return color;
}
)";

        struct Rect
        {
            std::int16_t x, y, w, h;
        };
        static_assert(sizeof(Rect) == 8);

        struct Vertex
        {
            float x, y, z, rhw, u, v;
        };
        constexpr DWORD vertex_format = D3DFVF_XYZRHW | D3DFVF_TEX1;

        using QueueBlurFn = void(__thiscall*)(void*, int, int, int, int);
        using PaintUiFn = void(__fastcall*)(void*, void*);
        using ReplayOverlayFn = void(__cdecl*)();
        using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT,
                                              ID3DBlob**, ID3DBlob**);

        const auto queue_blur = reinterpret_cast<QueueBlurFn>(queue_blur_address);
        const auto paint_ui = reinterpret_cast<PaintUiFn>(paint_ui_address);
        const auto replay_overlay = reinterpret_cast<ReplayOverlayFn>(replay_overlay_address);

        IDirect3DDevice9** device_slot = nullptr;

        // paint_ui runs on the game's thread, the replay on the render thread. Like the overlay, the replay draws the
        // latest frame's rects.
        std::mutex rects_mutex;
        std::vector<Rect> latest_rects;

        // Render thread only.
        std::vector<Rect> drawn_rects;
        IDirect3DPixelShader9* blur_shader = nullptr;
        IDirect3DDevice9* blur_shader_device = nullptr;
        bool blur_shader_failed = false;

        struct CallPatch
        {
            std::uintptr_t address;
            std::array<std::uint8_t, call_size> original{};
        };

        struct PatchState
        {
            std::mutex mutex;
            bool installed{};
            CallPatch blur_call{ blur_call_address };
            CallPatch paint_ui_call{ paint_ui_call_address };
            CallPatch replay_overlay_call{ replay_overlay_call_address };
        };

        PatchState patch_state;

        template <typename T>
        T& At(void* base, std::size_t offset) noexcept
        {
            return *reinterpret_cast<T*>(static_cast<std::uint8_t*>(base) + offset);
        }

        // __fastcall with the rect on the stack: ecx = context, retn 10h, like the game's __thiscall.
        void __fastcall QueueBlur(void* context, void*, int x, int y, int w, int h) noexcept
        {
            const auto begin = At<std::uintptr_t>(context, context_rects_begin);
            const auto end = At<std::uintptr_t>(context, context_rects_end);
            if ((end - begin) / sizeof(Rect) >= max_queued) return;
            queue_blur(context, x, y, w, h);
        }

        // Takes this frame's rects (in-game paint ran just before) off the list, so the game never draws them.
        void __fastcall PaintUi(void* renderer, void* frame) noexcept
        {
            // Not blur: the one per-frame hook on the game's thread, so the built-in Lua features get a state here.
            lua_guard::KeepStateAlive();
            paint_ui(renderer, frame);
            auto* const context = *reinterpret_cast<void**>(blur_context_address);
            if (context == nullptr) return;
            auto*& begin = At<Rect*>(context, context_rects_begin);
            auto*& end = At<Rect*>(context, context_rects_end);
            {
                std::scoped_lock lock(rects_mutex);
                try
                {
                    latest_rects.assign(begin, end);
                }
                catch (...)
                {
                    latest_rects.clear();
                }
            }
            end = begin;
        }

        IDirect3DPixelShader9* BlurShader(IDirect3DDevice9* device) noexcept
        {
            if (blur_shader_device == device && (blur_shader != nullptr || blur_shader_failed)) return blur_shader;
            if (blur_shader != nullptr) blur_shader->Release();
            blur_shader = nullptr;
            blur_shader_failed = true;
            blur_shader_device = device;

            const HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
            const auto compile = compiler ? reinterpret_cast<D3DCompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
            if (compile == nullptr) return nullptr;
            ID3DBlob* code = nullptr;
            ID3DBlob* errors = nullptr;
            if (SUCCEEDED(compile(blur_shader_source, std::strlen(blur_shader_source), nullptr, nullptr, nullptr, "main",
                                  "ps_2_0", 0, 0, &code, &errors)))
            {
                if (SUCCEEDED(device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &blur_shader)))
                    blur_shader_failed = false;
            }
            if (code != nullptr) code->Release();
            if (errors != nullptr) errors->Release();
            return blur_shader;
        }

        void DrawQuad(IDirect3DDevice9* device, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) noexcept
        {
            // Pixel centers are at .0 in Direct3D 9, texel centers at .5.
            const Vertex quad[4]{
                { x0 - 0.5f, y0 - 0.5f, 0.f, 1.f, u0, v0 },
                { x1 - 0.5f, y0 - 0.5f, 0.f, 1.f, u1, v0 },
                { x0 - 0.5f, y1 - 0.5f, 0.f, 1.f, u0, v1 },
                { x1 - 0.5f, y1 - 0.5f, 0.f, 1.f, u1, v1 },
            };
            device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
        }

        void SetStates(IDirect3DDevice9* device) noexcept
        {
            device->SetVertexShader(nullptr);
            device->SetFVF(vertex_format);
            device->SetRenderState(D3DRS_ZENABLE, FALSE);
            device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
            device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
            device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            device->SetRenderState(D3DRS_FOGENABLE, FALSE);
            device->SetRenderState(D3DRS_LIGHTING, FALSE);
            device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
            device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
            device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        }

        // One blur pass from source into target, across (horizontal) or down.
        void BlurPass(IDirect3DDevice9* device, IDirect3DPixelShader9* shader, IDirect3DTexture9* source,
                      IDirect3DTexture9* target, int width, int height, bool horizontal) noexcept
        {
            IDirect3DSurface9* surface = nullptr;
            if (FAILED(target->GetSurfaceLevel(0, &surface))) return;
            device->SetRenderTarget(0, surface);
            surface->Release();
            const float step[4]{ horizontal ? 1.f / width : 0.f, horizontal ? 0.f : 1.f / height, 0.f, 0.f };
            device->SetPixelShader(shader);
            device->SetPixelShaderConstantF(0, step, 1);
            device->SetTexture(0, source);
            DrawQuad(device, 0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 0.f, 1.f, 1.f);
        }

        // The textures are made for each frame that blurs: render targets have to be released before the game
        // resets the device (alt-tab, resolution change), and this way none are held.
        void DrawBlur(IDirect3DDevice9* device, const std::vector<Rect>& rects) noexcept
        {
            IDirect3DSurface9* back_buffer = nullptr;
            if (FAILED(device->GetRenderTarget(0, &back_buffer))) return;
            D3DSURFACE_DESC desc{};
            back_buffer->GetDesc(&desc);
            const int width = std::max(1, static_cast<int>(desc.Width) / downscale);
            const int height = std::max(1, static_cast<int>(desc.Height) / downscale);

            IDirect3DStateBlock9* state = nullptr;
            IDirect3DTexture9* blurred = nullptr;
            IDirect3DTexture9* scratch = nullptr;
            IDirect3DSurface9* blurred_surface = nullptr;
            const bool began_scene = SUCCEEDED(device->BeginScene());

            if (SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &state))
                && SUCCEEDED(device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT, &blurred, nullptr))
                && SUCCEEDED(device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT, &scratch, nullptr))
                && SUCCEEDED(blurred->GetSurfaceLevel(0, &blurred_surface))
                && SUCCEEDED(device->StretchRect(back_buffer, nullptr, blurred_surface, nullptr, D3DTEXF_LINEAR)))
            {
                SetStates(device);
                if (IDirect3DPixelShader9* const shader = BlurShader(device))
                {
                    for (int i = 0; i < blur_iterations; ++i)
                    {
                        BlurPass(device, shader, blurred, scratch, width, height, true);
                        BlurPass(device, shader, scratch, blurred, width, height, false);
                    }
                }

                device->SetRenderTarget(0, back_buffer);
                device->SetPixelShader(nullptr);
                device->SetTexture(0, blurred);
                device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
                const float screen_w = static_cast<float>(desc.Width);
                const float screen_h = static_cast<float>(desc.Height);
                for (const Rect& rect : rects)
                {
                    if (rect.w <= 0 || rect.h <= 0) continue;
                    const float x0 = rect.x, y0 = rect.y;
                    const float x1 = x0 + rect.w, y1 = y0 + rect.h;
                    DrawQuad(device, x0, y0, x1, y1, x0 / screen_w, y0 / screen_h, x1 / screen_w, y1 / screen_h);
                }
            }

            device->SetRenderTarget(0, back_buffer);
            if (state != nullptr)
            {
                state->Apply();
                state->Release();
            }
            if (began_scene) device->EndScene();
            if (blurred_surface != nullptr) blurred_surface->Release();
            if (scratch != nullptr) scratch->Release();
            if (blurred != nullptr) blurred->Release();
            back_buffer->Release();
        }

        void __cdecl ReplayOverlay() noexcept
        {
            {
                std::scoped_lock lock(rects_mutex);
                drawn_rects = latest_rects;
            }
            IDirect3DDevice9* const device = *device_slot;
            if (device != nullptr && !drawn_rects.empty()) DrawBlur(device, drawn_rects);
            replay_overlay();
        }

        bool WriteCode(std::uintptr_t address, const std::uint8_t* bytes, std::size_t size) noexcept
        {
            auto* target = reinterpret_cast<std::uint8_t*>(address);
            DWORD old_protection{};
            if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &old_protection)) return false;
            std::memcpy(target, bytes, size);
            DWORD ignored{};
            VirtualProtect(target, size, old_protection, &ignored);
            FlushInstructionCache(GetCurrentProcess(), target, size);
            return true;
        }

        bool WriteCall(CallPatch& patch, const void* destination) noexcept
        {
            std::array<std::uint8_t, call_size> call{ 0xE8 };
            const auto relative = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(destination) - (patch.address + call_size));
            std::memcpy(call.data() + 1, &relative, sizeof(relative));
            std::memcpy(patch.original.data(), reinterpret_cast<const void*>(patch.address), call_size);
            return WriteCode(patch.address, call.data(), call_size);
        }

        void RestoreCall(const CallPatch& patch) noexcept
        {
            WriteCode(patch.address, patch.original.data(), call_size);
        }

        template <std::size_t N>
        bool HasBytes(std::uintptr_t address, const std::array<std::uint8_t, N>& expected) noexcept
        {
            return std::memcmp(reinterpret_cast<const void*>(address), expected.data(), N) == 0;
        }

        IDirect3DDevice9** FindDeviceSlot() noexcept
        {
            const HMODULE module = GetModuleHandleA("shaderapidx9.dll");
            if (module == nullptr) return nullptr;
            std::uint8_t* const code = PatternScan(module, device_signature);
            return code ? *reinterpret_cast<IDirect3DDevice9***>(code + 1) : nullptr;
        }
    }

    bool Install() noexcept
    {
        std::scoped_lock lock(patch_state.mutex);
        if (patch_state.installed) return true;
        if (!HasBytes(blur_call_address - blur_call_offset, blur_call_site)
            || !HasBytes(paint_ui_call_address - paint_ui_call_offset, paint_ui_call_site)
            || !HasBytes(replay_overlay_call_address - replay_overlay_call_offset, replay_overlay_call_site))
            return false;
        device_slot = FindDeviceSlot();
        if (device_slot == nullptr) return false;

        if (!WriteCall(patch_state.blur_call, reinterpret_cast<const void*>(&QueueBlur))) return false;
        if (!WriteCall(patch_state.paint_ui_call, reinterpret_cast<const void*>(&PaintUi)))
        {
            RestoreCall(patch_state.blur_call);
            return false;
        }
        if (!WriteCall(patch_state.replay_overlay_call, reinterpret_cast<const void*>(&ReplayOverlay)))
        {
            RestoreCall(patch_state.paint_ui_call);
            RestoreCall(patch_state.blur_call);
            return false;
        }
        patch_state.installed = true;
        return true;
    }

    void Remove() noexcept
    {
        std::scoped_lock lock(patch_state.mutex);
        if (!patch_state.installed) return;
        RestoreCall(patch_state.replay_overlay_call);
        RestoreCall(patch_state.paint_ui_call);
        RestoreCall(patch_state.blur_call);
        patch_state.installed = false;
    }

    bool IsInstalled() noexcept
    {
        std::scoped_lock lock(patch_state.mutex);
        return patch_state.installed;
    }
#else
    bool Install() noexcept { return false; }
    void Remove() noexcept {}
    bool IsInstalled() noexcept { return false; }
#endif
}
