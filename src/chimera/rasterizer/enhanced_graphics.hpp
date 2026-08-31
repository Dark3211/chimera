// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_RASTERIZER_ENHANCED_GRAPHICS_HPP
#define CHIMERA_RASTERIZER_ENHANCED_GRAPHICS_HPP

#include <cstdint>
#include <cstring>
#include <d3d9.h>
#include <d3dcompiler.h>

#include "rasterizer.hpp"
#include "graphics_runtime_metrics.hpp"
#include "../chimera.hpp"
#include "../config/ini.hpp"
#include "../event/d3d9_end_scene.hpp"
#include "../event/d3d9_reset.hpp"
#include "../event/game_loop.hpp"
#include "../halo_data/game_engine.hpp"
#include "../output/output.hpp"
#include "../signature/hook.hpp"

namespace Chimera {
    namespace EnhancedGraphics {
        struct Settings {
            bool enabled = false;
            bool fxaa = false;
            bool smaa_exclude_hud = true;
            bool color_correction = false;
            float sharpening = 0.0f;
            float brightness = 1.0f;
            float contrast = 1.0f;
            float saturation = 1.0f;
        };

        struct State {
            Settings settings {};
            IDirect3DDevice9 *device = nullptr;
            IDirect3DTexture9 *frame_texture = nullptr;
            IDirect3DSurface9 *frame_surface = nullptr;
            IDirect3DPixelShader9 *pixel_shader = nullptr;
            UINT width = 0;
            UINT height = 0;
            D3DFORMAT format = D3DFMT_UNKNOWN;
            bool runtime_disabled = false;
            bool failure_reported = false;
            bool processing_frame = false;
            std::uint32_t tracked_resources = 0;
        };

        struct ScreenVertex {
            float x;
            float y;
            float z;
            float rhw;
            float u;
            float v;
        };

        static constexpr DWORD SCREEN_VERTEX_FVF = D3DFVF_XYZRHW | D3DFVF_TEX1;

        inline State &state() noexcept {
            static State instance;
            return instance;
        }

        inline float clamp_setting(double value, float minimum, float maximum) noexcept {
            const float converted = static_cast<float>(value);
            return converted < minimum ? minimum : (converted > maximum ? maximum : converted);
        }

        template<typename T> inline void release_com(T *&resource) noexcept {
            if(resource) {
                resource->Release();
                resource = nullptr;
            }
        }

        inline void release_resources() noexcept {
            auto &graphics = state();
            if(graphics.tracked_resources != 0) {
                GraphicsRuntimeMetrics::resources_released(graphics.tracked_resources);
                graphics.tracked_resources = 0;
            }
            release_com(graphics.pixel_shader);
            release_com(graphics.frame_surface);
            release_com(graphics.frame_texture);
            graphics.device = nullptr;
            graphics.width = 0;
            graphics.height = 0;
            graphics.format = D3DFMT_UNKNOWN;
            graphics.processing_frame = false;
        }

        inline void report_failure_once(const char *message) noexcept {
            auto &graphics = state();
            if(!graphics.failure_reported) {
                console_error(message);
                graphics.failure_reported = true;
            }
        }

        inline void disable_for_session(const char *reason) noexcept {
            auto &graphics = state();
            report_failure_once(reason);
            release_resources();
            graphics.runtime_disabled = true;
        }

        inline Settings load_settings() noexcept {
            Settings settings;
            const auto *ini = get_chimera().get_ini();
            if(!ini) {
                return settings;
            }

            settings.enabled = ini->get_value_bool("graphics.enabled").value_or(false);

            if(const char *anti_aliasing = ini->get_value("graphics.anti_aliasing")) {
                settings.fxaa = std::strcmp(anti_aliasing, "fxaa") == 0 || std::strcmp(anti_aliasing, "FXAA") == 0;
            }
            settings.smaa_exclude_hud = ini->get_value_bool("graphics.smaa_exclude_hud").value_or(true);

            if(ini->get_value_bool("graphics.sharpening").value_or(false)) {
                settings.sharpening = clamp_setting(
                    ini->get_value_float("graphics.sharpening_strength").value_or(0.15),
                    0.0f,
                    1.0f
                );
            }

            settings.color_correction = ini->get_value_bool("graphics.color_correction").value_or(false);
            settings.brightness = clamp_setting(
                ini->get_value_float("graphics.brightness").value_or(1.0),
                0.5f,
                1.5f
            );
            settings.contrast = clamp_setting(
                ini->get_value_float("graphics.contrast").value_or(1.0),
                0.5f,
                1.5f
            );
            settings.saturation = clamp_setting(
                ini->get_value_float("graphics.saturation").value_or(1.0),
                0.0f,
                2.0f
            );
            return settings;
        }

        inline bool additional_post_effects_enabled() noexcept {
            const auto *ini = get_chimera().get_ini();
            return ini && (
                ini->get_value_bool("graphics.adaptive_sharpening").value_or(false) ||
                ini->get_value_bool("graphics.sharpening_anti_halo").value_or(false) ||
                ini->get_value_bool("graphics.bloom").value_or(false) ||
                ini->get_value_bool("graphics.debanding").value_or(false) ||
                ini->get_value_bool("graphics.dithering").value_or(false)
            );
        }

        inline bool effects_enabled(const Settings &settings) noexcept {
            return settings.fxaa || settings.sharpening > 0.0f || settings.color_correction ||
                   additional_post_effects_enabled();
        }

        inline bool strict_pre_hud_mode(const Settings &settings) noexcept {
            return settings.fxaa && settings.smaa_exclude_hud;
        }

        static constexpr const char *PASS_THROUGH_SHADER = R"HLSL(
            sampler2D frame_sampler : register(s0);
            float4 main(float2 uv : TEXCOORD0) : COLOR0 {
                return tex2D(frame_sampler, uv);
            }
        )HLSL";

        static constexpr const char *POST_PROCESS_SHADER = R"HLSL(
            sampler2D frame_sampler : register(s0);

            // c0: inverse width, inverse height, FXAA enabled, sharpening strength
            float4 frame_options : register(c0);
            // c1: brightness, contrast, saturation, color correction enabled
            float4 color_options : register(c1);

            float luma(float3 color) {
                return dot(color, float3(0.299, 0.587, 0.114));
            }

            float4 main(float2 uv : TEXCOORD0) : COLOR0 {
                const float2 texel = frame_options.xy;
                const float4 center_sample = tex2D(frame_sampler, uv);
                const float3 center = center_sample.rgb;

                const float3 rgb_l = tex2D(frame_sampler, uv + float2(-texel.x, 0.0)).rgb;
                const float3 rgb_r = tex2D(frame_sampler, uv + float2( texel.x, 0.0)).rgb;
                const float3 rgb_u = tex2D(frame_sampler, uv + float2(0.0, -texel.y)).rgb;
                const float3 rgb_d = tex2D(frame_sampler, uv + float2(0.0,  texel.y)).rgb;
                const float3 cross_average = 0.25 * (rgb_l + rgb_r + rgb_u + rgb_d);

                const float3 rgb_nw = tex2D(frame_sampler, uv + float2(-1.0, -1.0) * texel).rgb;
                const float3 rgb_ne = tex2D(frame_sampler, uv + float2( 1.0, -1.0) * texel).rgb;
                const float3 rgb_sw = tex2D(frame_sampler, uv + float2(-1.0,  1.0) * texel).rgb;
                const float3 rgb_se = tex2D(frame_sampler, uv + float2( 1.0,  1.0) * texel).rgb;

                const float luma_nw = luma(rgb_nw);
                const float luma_ne = luma(rgb_ne);
                const float luma_sw = luma(rgb_sw);
                const float luma_se = luma(rgb_se);
                const float luma_m = luma(center);
                const float luma_min = min(luma_m, min(min(luma_nw, luma_ne), min(luma_sw, luma_se)));
                const float luma_max = max(luma_m, max(max(luma_nw, luma_ne), max(luma_sw, luma_se)));
                const float luma_range = max(luma_max - luma_min, 0.0001);

                float2 direction;
                direction.x = -((luma_nw + luma_ne) - (luma_sw + luma_se));
                direction.y =  ((luma_nw + luma_sw) - (luma_ne + luma_se));

                const float direction_reduce = max(
                    (luma_nw + luma_ne + luma_sw + luma_se) * (0.25 / 8.0),
                    1.0 / 128.0
                );
                const float reciprocal_minimum = 1.0 / (min(abs(direction.x), abs(direction.y)) + direction_reduce);
                direction = clamp(direction * reciprocal_minimum, float2(-8.0, -8.0), float2(8.0, 8.0)) * texel;

                const float3 rgb_a = 0.5 * (
                    tex2D(frame_sampler, uv + direction * (1.0 / 3.0 - 0.5)).rgb +
                    tex2D(frame_sampler, uv + direction * (2.0 / 3.0 - 0.5)).rgb
                );
                const float3 rgb_b = rgb_a * 0.5 + 0.25 * (
                    tex2D(frame_sampler, uv + direction * -0.5).rgb +
                    tex2D(frame_sampler, uv + direction *  0.5).rgb
                );
                const float luma_b = luma(rgb_b);
                const float use_rgb_b = step(luma_min, luma_b) * step(luma_b, luma_max);
                const float3 fxaa_color = lerp(rgb_a, rgb_b, use_rgb_b);

                // HUD/text is excluded structurally in strict pre-HUD mode, so FXAA can
                // treat thin world geometry like branches, cables and distant silhouettes
                // as real edges instead of protecting them as possible glyphs.
                const float edge_strength = saturate((luma_range - 0.018) * 10.0);
                const float fxaa_amount = saturate(frame_options.z) * edge_strength;

                float3 color = lerp(center, fxaa_color, fxaa_amount);

                const float sharpen_gain = saturate(frame_options.w) * 2.5;
                const float detail_limit = 0.03 + saturate(luma_range) * 0.20;
                const float3 sharp_detail = clamp(color - cross_average, -detail_limit, detail_limit);
                color = saturate(color + sharp_detail * sharpen_gain);

                const float grey = luma(color);
                float3 corrected = lerp(float3(grey, grey, grey), color, color_options.z);
                corrected = (corrected - 0.5) * color_options.y + 0.5;
                corrected *= color_options.x;
                color = lerp(color, saturate(corrected), saturate(color_options.w));

                return float4(color, center_sample.a);
            }
        )HLSL";

        inline bool create_shader(IDirect3DDevice9 *device) noexcept {
            auto &graphics = state();
            if(graphics.pixel_shader) {
                return true;
            }
            if(!device || !d3d9_device_caps || d3d9_device_caps->PixelShaderVersion < 0xffff0200) {
                return false;
            }

            const char *profile = d3d9_device_caps->PixelShaderVersion >= 0xffff0300 ? "ps_3_0" : "ps_2_0";
            const char *source = effects_enabled(graphics.settings) ? POST_PROCESS_SHADER : PASS_THROUGH_SHADER;
            ID3DBlob *compiled_shader = nullptr;
            if(!rasterizer_compile_shader(source, "main", profile, nullptr, &compiled_shader) || !compiled_shader) {
                release_com(compiled_shader);
                return false;
            }

            const HRESULT result = IDirect3DDevice9_CreatePixelShader(
                device,
                reinterpret_cast<const DWORD *>(compiled_shader->GetBufferPointer()),
                &graphics.pixel_shader
            );
            release_com(compiled_shader);
            return SUCCEEDED(result) && graphics.pixel_shader;
        }

        inline bool create_frame_texture(IDirect3DDevice9 *device, const D3DSURFACE_DESC &description) noexcept {
            auto &graphics = state();
            const HRESULT texture_result = IDirect3DDevice9_CreateTexture(
                device,
                description.Width,
                description.Height,
                1,
                D3DUSAGE_RENDERTARGET,
                description.Format,
                D3DPOOL_DEFAULT,
                &graphics.frame_texture,
                nullptr
            );
            if(FAILED(texture_result) || !graphics.frame_texture) {
                return false;
            }

            const HRESULT surface_result = IDirect3DTexture9_GetSurfaceLevel(graphics.frame_texture, 0, &graphics.frame_surface);
            if(FAILED(surface_result) || !graphics.frame_surface) {
                release_com(graphics.frame_texture);
                return false;
            }

            graphics.device = device;
            graphics.width = description.Width;
            graphics.height = description.Height;
            graphics.format = description.Format;
            return true;
        }

        inline bool ensure_resources(IDirect3DDevice9 *device, const D3DSURFACE_DESC &description) noexcept {
            auto &graphics = state();
            const bool resources_match =
                graphics.device == device &&
                graphics.width == description.Width &&
                graphics.height == description.Height &&
                graphics.format == description.Format &&
                graphics.frame_texture &&
                graphics.frame_surface &&
                graphics.pixel_shader;

            if(resources_match) {
                return true;
            }

            release_resources();
            graphics.device = device;
            if(!create_shader(device) || !create_frame_texture(device, description)) {
                release_resources();
                return false;
            }
            graphics.tracked_resources = 3;
            GraphicsRuntimeMetrics::resources_created(graphics.tracked_resources);
            GraphicsRuntimeMetrics::subsystem_recovered(GraphicsRuntimeMetrics::Subsystem::ENHANCED);
            return true;
        }

        inline void on_reset(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *) noexcept {
            GraphicsRuntimeMetrics::subsystem_reset(GraphicsRuntimeMetrics::Subsystem::ENHANCED);
            release_resources();
            GraphicsRuntimeMetrics::write_log();
        }

        inline bool capture_state(
            IDirect3DDevice9 *device,
            IDirect3DStateBlock9 **state_block,
            IDirect3DSurface9 **render_target,
            D3DVIEWPORT9 &viewport,
            RECT &scissor
        ) noexcept {
            if(FAILED(IDirect3DDevice9_CreateStateBlock(device, D3DSBT_ALL, state_block)) || !*state_block) {
                return false;
            }
            if(FAILED(IDirect3DStateBlock9_Capture(*state_block))) {
                release_com(*state_block);
                return false;
            }
            if(FAILED(IDirect3DDevice9_GetRenderTarget(device, 0, render_target)) || !*render_target) {
                release_com(*state_block);
                return false;
            }
            if(FAILED(IDirect3DDevice9_GetViewport(device, &viewport))) {
                release_com(*render_target);
                release_com(*state_block);
                return false;
            }
            if(FAILED(IDirect3DDevice9_GetScissorRect(device, &scissor))) {
                scissor.left = 0;
                scissor.top = 0;
                scissor.right = static_cast<LONG>(state().width);
                scissor.bottom = static_cast<LONG>(state().height);
            }
            return true;
        }

        inline void restore_state(
            IDirect3DDevice9 *device,
            IDirect3DStateBlock9 *state_block,
            IDirect3DSurface9 *render_target,
            const D3DVIEWPORT9 &viewport,
            const RECT &scissor
        ) noexcept {
            if(state_block) {
                IDirect3DStateBlock9_Apply(state_block);
            }
            if(render_target) {
                IDirect3DDevice9_SetRenderTarget(device, 0, render_target);
            }
            IDirect3DDevice9_SetViewport(device, &viewport);
            IDirect3DDevice9_SetScissorRect(device, &scissor);

            release_com(render_target);
            release_com(state_block);
        }

        inline bool draw_postprocess_in_active_scene(
            IDirect3DDevice9 *device,
            IDirect3DSurface9 *render_target,
            const D3DSURFACE_DESC &description
        ) noexcept {
            auto &graphics = state();
            if(!device || !render_target || !graphics.frame_texture || !graphics.pixel_shader) {
                return false;
            }

            bool can_draw = SUCCEEDED(IDirect3DDevice9_SetRenderTarget(device, 0, render_target));

            D3DVIEWPORT9 viewport {};
            viewport.X = 0;
            viewport.Y = 0;
            viewport.Width = description.Width;
            viewport.Height = description.Height;
            viewport.MinZ = 0.0f;
            viewport.MaxZ = 1.0f;
            if(can_draw && FAILED(IDirect3DDevice9_SetViewport(device, &viewport))) {
                can_draw = false;
            }

            if(can_draw) {
                IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, FALSE);
                IDirect3DDevice9_SetRenderState(device, D3DRS_ZWRITEENABLE, FALSE);
                IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHABLENDENABLE, FALSE);
                IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHATESTENABLE, FALSE);
                IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
                IDirect3DDevice9_SetRenderState(device, D3DRS_SCISSORTESTENABLE, FALSE);
                IDirect3DDevice9_SetRenderState(device, D3DRS_FOGENABLE, FALSE);
                IDirect3DDevice9_SetRenderState(device, D3DRS_SRGBWRITEENABLE, FALSE);

                IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
                IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_SRGBTEXTURE, FALSE);

                IDirect3DDevice9_SetVertexShader(device, nullptr);
                IDirect3DDevice9_SetPixelShader(device, graphics.pixel_shader);
                IDirect3DDevice9_SetFVF(device, SCREEN_VERTEX_FVF);
                IDirect3DDevice9_SetTexture(device, 0, reinterpret_cast<IDirect3DBaseTexture9 *>(graphics.frame_texture));

                // Upload c0 and c1 in a single contiguous write. This is both simpler and
                // avoids wrapper-specific issues where a second SetPixelShaderConstantF
                // call for c1 may be dropped even though c0 succeeds.
                const float shader_options[8] = {
                    1.0f / static_cast<float>(description.Width),
                    1.0f / static_cast<float>(description.Height),
                    graphics.settings.fxaa ? 1.0f : 0.0f,
                    graphics.settings.sharpening,
                    graphics.settings.brightness,
                    graphics.settings.contrast,
                    graphics.settings.saturation,
                    graphics.settings.color_correction ? 1.0f : 0.0f
                };
                if(FAILED(IDirect3DDevice9_SetPixelShaderConstantF(device, 0, shader_options, 2))) {
                    report_failure_once("Chimera Graphics: pixel shader constant upload failed; post-process draw skipped.");
                    can_draw = false;
                }

                const float right = static_cast<float>(description.Width) - 0.5f;
                const float bottom = static_cast<float>(description.Height) - 0.5f;
                const ScreenVertex vertices[4] = {
                    { -0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f },
                    {  right, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f },
                    { -0.5f,  bottom, 0.0f, 1.0f, 0.0f, 1.0f },
                    {  right,  bottom, 0.0f, 1.0f, 1.0f, 1.0f }
                };

                if(can_draw) {
                    can_draw = SUCCEEDED(IDirect3DDevice9_DrawPrimitiveUP(
                        device,
                        D3DPT_TRIANGLESTRIP,
                        2,
                        vertices,
                        sizeof(ScreenVertex)
                    ));
                }
            }

            IDirect3DDevice9_SetTexture(device, 0, nullptr);
            return can_draw;
        }

        inline void on_end_scene_after(IDirect3DDevice9 *device) noexcept {
            auto &graphics = state();
            if(!graphics.settings.enabled || graphics.runtime_disabled || graphics.processing_frame ||
               strict_pre_hud_mode(graphics.settings) || !device) {
                return;
            }

            struct ProcessingGuard {
                bool &processing;
                explicit ProcessingGuard(bool &value) : processing(value) { processing = true; }
                ~ProcessingGuard() { processing = false; }
            } guard(graphics.processing_frame);

            IDirect3DSurface9 *back_buffer = nullptr;
            if(FAILED(IDirect3DDevice9_GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer)) || !back_buffer) {
                return;
            }

            D3DSURFACE_DESC description {};
            if(FAILED(IDirect3DSurface9_GetDesc(back_buffer, &description)) ||
               description.Width == 0 || description.Height == 0 || description.Format == D3DFMT_UNKNOWN) {
                release_com(back_buffer);
                return;
            }

            if(!ensure_resources(device, description)) {
                release_com(back_buffer);
                disable_for_session("Chimera Graphics disabled for this session: D3D9 post-process resources could not be created.");
                return;
            }

            if(FAILED(IDirect3DDevice9_StretchRect(device, back_buffer, nullptr, graphics.frame_surface, nullptr, D3DTEXF_NONE))) {
                release_com(back_buffer);
                return;
            }

            IDirect3DStateBlock9 *state_block = nullptr;
            IDirect3DSurface9 *old_render_target = nullptr;
            D3DVIEWPORT9 old_viewport {};
            RECT old_scissor {};
            if(!capture_state(device, &state_block, &old_render_target, old_viewport, old_scissor)) {
                release_com(back_buffer);
                return;
            }

            if(FAILED(IDirect3DDevice9_BeginScene(device))) {
                restore_state(device, state_block, old_render_target, old_viewport, old_scissor);
                release_com(back_buffer);
                return;
            }

            draw_postprocess_in_active_scene(device, back_buffer, description);
            restore_state(device, state_block, old_render_target, old_viewport, old_scissor);
            IDirect3DDevice9_EndScene(device);
            release_com(back_buffer);
        }

        inline std::byte *validated_pre_hud_call_site() noexcept {
            // Cache the successfully validated CE call site before the hook rewrites it.
            // Graphics diagnostics run after SMAA setup. Without this cache, diagnostics
            // re-check the original CALL bytes after write_jmp_call() has already changed
            // them and can falsely report pre_hud_validated=0 / smaa_active=0.
            static std::byte *cached_call_site = nullptr;

            if(game_engine() != GameEngine::GAME_ENGINE_CUSTOM_EDITION) {
                return nullptr;
            }
            if(cached_call_site) {
                return cached_call_site;
            }

            auto *module = reinterpret_cast<std::byte *>(GetModuleHandle(nullptr));
            if(!module) {
                return nullptr;
            }

            auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(module);
            if(dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
                return nullptr;
            }
            auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(module + dos->e_lfanew);
            if(nt->Signature != IMAGE_NT_SIGNATURE) {
                return nullptr;
            }

            // Halo Custom Edition 1.10. Chimera itself requires 1.10 for the normal client.
            // Keep the original strict address/target validation unchanged. The only
            // behavioral change here is caching a successful result for later diagnostics.
            constexpr std::uintptr_t PRE_HUD_CALL_RVA = 0x10FE61;
            constexpr std::uintptr_t INTERFACE_DRAW_SCREEN_RVA = 0x0974F0;
            constexpr std::uintptr_t UI_WIDGETS_CALL_RVA = 0x10FE6D;
            constexpr std::uintptr_t RENDER_UI_WIDGETS_RVA = 0x09B450;

            if(nt->OptionalHeader.SizeOfImage <= UI_WIDGETS_CALL_RVA + 5) {
                return nullptr;
            }

            auto validate_relative_call = [](std::byte *site, std::byte *expected_target) noexcept {
                if(*reinterpret_cast<const std::uint8_t *>(site) != 0xE8) {
                    return false;
                }
                std::int32_t displacement = 0;
                std::memcpy(&displacement, site + 1, sizeof(displacement));
                return site + 5 + displacement == expected_target;
            };

            auto *pre_hud_call = module + PRE_HUD_CALL_RVA;
            auto *interface_draw_screen = module + INTERFACE_DRAW_SCREEN_RVA;
            auto *ui_widgets_call = module + UI_WIDGETS_CALL_RVA;
            auto *render_ui_widgets = module + RENDER_UI_WIDGETS_RVA;

            if(!validate_relative_call(pre_hud_call, interface_draw_screen) ||
               !validate_relative_call(ui_widgets_call, render_ui_widgets)) {
                return nullptr;
            }

            cached_call_site = pre_hud_call;
            return cached_call_site;
        }

        inline void on_pre_hud() noexcept {
            auto &graphics = state();
            if(!graphics.settings.enabled || graphics.runtime_disabled || graphics.processing_frame ||
               !strict_pre_hud_mode(graphics.settings) || !global_d3d9_device || !*global_d3d9_device) {
                return;
            }

            auto *device = *global_d3d9_device;

            struct ProcessingGuard {
                bool &processing;
                explicit ProcessingGuard(bool &value) : processing(value) { processing = true; }
                ~ProcessingGuard() { processing = false; }
            } guard(graphics.processing_frame);

            // At INTERFACE_DRAW_SCREEN Halo may still be rendering the 3D world into an
            // off-screen render target. Processing the swap-chain backbuffer here can be
            // overwritten by Halo's later scene composition. Capture the active target
            // instead; the HUD is drawn only after this hook returns.
            IDirect3DStateBlock9 *state_block = nullptr;
            IDirect3DSurface9 *scene_render_target = nullptr;
            D3DVIEWPORT9 old_viewport {};
            RECT old_scissor {};
            if(!capture_state(device, &state_block, &scene_render_target, old_viewport, old_scissor)) {
                return;
            }

            D3DSURFACE_DESC description {};
            if(FAILED(IDirect3DSurface9_GetDesc(scene_render_target, &description)) ||
               description.Width == 0 || description.Height == 0 || description.Format == D3DFMT_UNKNOWN) {
                release_com(scene_render_target);
                release_com(state_block);
                return;
            }

            if(!ensure_resources(device, description)) {
                release_com(scene_render_target);
                release_com(state_block);
                disable_for_session("Chimera Graphics strict HUD exclusion disabled: active scene resources could not be created.");
                return;
            }

            // INTERFACE_DRAW_SCREEN is called while Halo's normal scene is active.
            // Close it temporarily so StretchRect can copy/resolve the active world target,
            // then immediately reopen the scene. Halo draws HUD/text afterwards and later
            // performs its normal EndScene, keeping BeginScene/EndScene balanced.
            if(FAILED(IDirect3DDevice9_EndScene(device))) {
                release_com(scene_render_target);
                release_com(state_block);
                return;
            }

            const bool copied = SUCCEEDED(IDirect3DDevice9_StretchRect(
                device,
                scene_render_target,
                nullptr,
                graphics.frame_surface,
                nullptr,
                D3DTEXF_NONE
            ));

            if(FAILED(IDirect3DDevice9_BeginScene(device))) {
                release_com(scene_render_target);
                release_com(state_block);
                disable_for_session("Chimera Graphics strict HUD exclusion disabled: D3D9 could not resume the Halo scene.");
                return;
            }

            if(copied) {
                draw_postprocess_in_active_scene(device, scene_render_target, description);
            }
            else {
                report_failure_once("Chimera Graphics strict HUD exclusion: active scene target could not be copied.");
            }

            restore_state(device, state_block, scene_render_target, old_viewport, old_scissor);
        }

        inline bool install_pre_hud_hook() noexcept {
            static Hook hook;
            if(hook.address && hook.hook && !hook.original_bytes.empty()) {
                return true;
            }

            auto *call_site = validated_pre_hud_call_site();
            if(!call_site) {
                return false;
            }

            write_jmp_call(call_site, hook, reinterpret_cast<const void *>(on_pre_hud));
            return hook.address == call_site && hook.hook && !hook.original_bytes.empty();
        }

        inline void set_up() noexcept {
            auto &graphics = state();
            graphics.settings = load_settings();
            if(!graphics.settings.enabled) {
                return;
            }

            if(strict_pre_hud_mode(graphics.settings)) {
                if(!install_pre_hud_hook()) {
                    disable_for_session("Chimera Graphics strict HUD exclusion is unavailable for this Halo executable; full-frame FXAA was not used as a fallback.");
                    return;
                }
            }
            else {
                add_d3d9_end_scene_after_event(on_end_scene_after, EVENT_PRIORITY_AFTER);
            }

            add_d3d9_reset_event(on_reset, EVENT_PRIORITY_BEFORE);
            add_game_exit_event(release_resources, EVENT_PRIORITY_BEFORE);
        }
    }

    inline void set_up_enhanced_graphics() noexcept {
        EnhancedGraphics::set_up();
    }
}

#endif