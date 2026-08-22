// SPDX-License-Identifier: GPL-3.0-only

#include <cstring>

#include "rasterizer.hpp"
#include "rasterizer_transparent_geometry.hpp"
#include "d3d9_model_shader_compat.hpp"
#include "d3d9_model_shader_primary_v2.hpp"
#include "d3d9_model_vertex_input_diag.hpp"
#include "d3d9_transparent_shader_compat.hpp"
#include "d3d9_modern_shader_bank.hpp"
#include "../chimera.hpp"
#include "../config/ini.hpp"
#include "../signature/hook.hpp"
#include "../halo_data/game_variables.hpp"
#include "../halo_data/shaders/shader_blob.hpp"
#include "../output/error_box.hpp"
#include "../event/game_loop.hpp"
#include "../event/d3d9_end_scene.hpp"


namespace Chimera {

    IDirect3DDevice9 **global_d3d9_device = nullptr;
    D3DCAPS9 *d3d9_device_caps = nullptr;
    bool chimera_rasterizer_enabled = false;

    IDirect3DPixelShader9 *chimera_pixel_shaders[NUMBER_OF_CHIMERA_PIXEL_SHADERS] = { nullptr };

    static bool validated_hybrid_auto_enabled = true;
    static bool validated_hybrid_announced = false;
    static unsigned validated_hybrid_retry_cooldown = 0;

    static bool d3d9on12_requested_now() noexcept {
        auto *backend = get_chimera().get_ini()->get_value("video_mode.d3d_backend");
        return backend
            && (_stricmp(backend, "9on12") == 0 || _stricmp(backend, "d3d9on12") == 0);
    }

    static bool validated_hybrid_requested() noexcept {
        if(!d3d9on12_requested_now() || !validated_hybrid_auto_enabled) {
            return false;
        }

        // Hybrid is the default for 9On12 on this hardened branch. A user can
        // explicitly fall back to the old path without removing any fixes.
        auto *mode = get_chimera().get_ini()->get_value("video_mode.d3d_shader_backend");
        if(!mode || !*mode) {
            return true;
        }
        if(_stricmp(mode, "off") == 0
            || _stricmp(mode, "stock") == 0
            || _stricmp(mode, "legacy") == 0) {
            return false;
        }
        return _stricmp(mode, "hybrid") == 0
            || _stricmp(mode, "validated") == 0
            || _stricmp(mode, "modern") == 0;
    }

    static bool refresh_hybrid_transparent_baselines() noexcept {
        using namespace D3D9TransparentShaderCompat;
        if(!vertex_shaders) {
            return false;
        }

        auto *generic_slot = vertex_shaders[VSH_TRANSPARENT_GENERIC_M].shader;
        auto *plasma_slot = vertex_shaders[VSH_TRANSPARENT_PLASMA_M].shader;
        if(!generic_slot || !plasma_slot) {
            return false;
        }

        // Halo can recreate its shader table on a map transition while the D3D9
        // device itself remains alive. In that case our AddRef'd stock pointer is
        // from the previous table generation. Recognize our own installed shader
        // first; otherwise recapture the current table entry as the new stock
        // baseline instead of treating it as a foreign/unsafe replacement.
        if(exact_generic_m && generic_slot == exact_generic_m) {
            generic_mode = GENERIC_MODE_EXACT_VS2;
        }
        else if(legacy_generic_m && generic_slot == legacy_generic_m) {
            generic_mode = GENERIC_MODE_LEGACY_VS3;
        }
        else {
            if(stock_generic_m != generic_slot) {
                if(stock_generic_m) {
                    stock_generic_m->Release();
                }
                stock_generic_m = generic_slot;
                stock_generic_m->AddRef();
            }
            generic_mode = GENERIC_MODE_STOCK;
        }

        if(exact_plasma_m && plasma_slot == exact_plasma_m) {
            plasma_m_active = true;
        }
        else {
            if(stock_plasma_m != plasma_slot) {
                if(stock_plasma_m) {
                    stock_plasma_m->Release();
                }
                stock_plasma_m = plasma_slot;
                stock_plasma_m->AddRef();
            }
            plasma_m_active = false;
        }

        return true;
    }

    static void ensure_validated_hybrid(IDirect3DDevice9 *device) noexcept {
        if(!device || !validated_hybrid_requested()) {
            return;
        }

        if(validated_hybrid_retry_cooldown > 0) {
            validated_hybrid_retry_cooldown--;
            return;
        }

        // Validated combination:
        //   MODEL_FOGGED..MODEL_ZBUFFER -> stock-equivalent VS3 bank
        //   TRANSPARENT_GENERIC_M       -> exact VS2 compatibility shader
        //   TRANSPARENT_PLASMA_M        -> exact VS2 compatibility shader
        // This exact combination restored model colour/lighting while removing
        // both known D3D9On12 spikes in live A/B testing.
        rasterizer_set_modern_model_test(true);

        if(!refresh_hybrid_transparent_baselines()) {
            validated_hybrid_retry_cooldown = 60;
            return;
        }

        using namespace D3D9TransparentShaderCompat;
        bool generic_ok = exact_generic_m
            && vertex_shaders[VSH_TRANSPARENT_GENERIC_M].shader == exact_generic_m;
        if(!generic_ok) {
            generic_ok = enable_generic_m_exact();
        }

        bool plasma_ok = exact_plasma_m
            && vertex_shaders[VSH_TRANSPARENT_PLASMA_M].shader == exact_plasma_m;
        if(!plasma_ok) {
            plasma_ok = enable_plasma_m_exact();
        }

        if(generic_ok && plasma_ok) {
            validated_hybrid_retry_cooldown = 0;
            if(!validated_hybrid_announced) {
                console_output(
                    "D3D9On12 validated hybrid active: MODEL=VS3, GENERIC_M=exact VS2, PLASMA_M=exact VS2."
                );
                validated_hybrid_announced = true;
            }
        }
        else {
            // Do not flood the in-game console if the shader table is between
            // generations for a few frames. Retry at a low rate until stable.
            validated_hybrid_retry_cooldown = 60;
        }
    }

    static void set_up_validated_hybrid() noexcept {
        validated_hybrid_announced = false;
        validated_hybrid_retry_cooldown = 0;
        if(global_d3d9_device && *global_d3d9_device) {
            ensure_validated_hybrid(*global_d3d9_device);
        }
    }

    static void set_validated_hybrid(bool enabled) noexcept {
        validated_hybrid_auto_enabled = enabled;
        validated_hybrid_announced = false;
        validated_hybrid_retry_cooldown = 0;

        if(enabled) {
            set_up_validated_hybrid();
            console_output("D3D9On12 validated hybrid: ON.");
        }
        else {
            rasterizer_set_modern_model_test(false);
            D3D9TransparentShaderCompat::restore_generic_m();
            D3D9TransparentShaderCompat::restore_plasma_m();
            console_output("D3D9On12 validated hybrid: OFF (manual/fallback control restored).");
        }
    }

    void rasterizer_set_render_state(D3DRENDERSTATETYPE state, DWORD value) noexcept {
        throw_error(global_d3d9_device, "d3d device missing");
        IDirect3DDevice9_SetRenderState(*global_d3d9_device, state, value);
    }

    void rasterizer_set_sampler_state(std::uint16_t sampler, D3DSAMPLERSTATETYPE type, DWORD value) noexcept {
        throw_error(global_d3d9_device, "d3d device missing");
        IDirect3DDevice9_SetSamplerState(*global_d3d9_device, sampler, type, value);
    }

    void rasterizer_create_pixel_shaders() noexcept {
        throw_error(global_d3d9_device, "d3d device missing");
        for(int i = 0; i < NUMBER_OF_CHIMERA_PIXEL_SHADERS; i++) {
            throw_error(!chimera_pixel_shaders[i], "Something went horribly wrong");
        }

        auto create_pixel_shader = [](const void *shader_bytecode, IDirect3DPixelShader9 **shader) noexcept {
            if(!shader) {
                return false;
            }
            *shader = nullptr;
            if(!shader_bytecode || !global_d3d9_device || !*global_d3d9_device) {
                return false;
            }
            const HRESULT result = IDirect3DDevice9_CreatePixelShader(
                *global_d3d9_device,
                reinterpret_cast<const DWORD *>(shader_bytecode),
                shader
            );
            if(FAILED(result) || !*shader) {
                *shader = nullptr;
                return false;
            }
            return true;
        };

        create_pixel_shader(white_1_1, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_WHITE_1_1]);

        // Ensure ps2.0 support for all except the ps_1_1 shader.
        if(!d3d9_device_caps || d3d9_device_caps->PixelShaderVersion < 0xffff0200) {
            return;
        }

        create_pixel_shader(white, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_WHITE]);
        create_pixel_shader(black, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_BLACK]);
        create_pixel_shader(hud_meters, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_HUD_METERS]);
        create_pixel_shader(fog, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_FOG]);
        create_pixel_shader(fog_akill, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_FOG_ALPHA_KILL]);
        create_pixel_shader(fog_screen, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_FOG_SCREEN]);
        create_pixel_shader(eff_nlin_tint_z, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NLIN_TINT_Z]);
        create_pixel_shader(eff_nlin_tint_add_z, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NLIN_TINT_ADD_Z]);
        create_pixel_shader(eff_nlin_tint_alpha_blend_z, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NLIN_TINT_ALPHA_BLEND_Z]);
        create_pixel_shader(eff_nlin_tint_double_mul_z, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NLIN_TINT_DOUBLE_MUL_Z]);
        create_pixel_shader(eff_nlin_tint_mul_z, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NLIN_TINT_MUL_Z]);
        create_pixel_shader(eff_nlin_tint_mul_add_z, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NLIN_TINT_MUL_ADD_Z]);
        create_pixel_shader(eff_normal_tint_z, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NORMAL_TINT_Z]);
        create_pixel_shader(eff_normal_tint_add_z, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NORMAL_TINT_ADD_Z]);
        create_pixel_shader(eff_normal_tint_alpha_blend_z, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NORMAL_TINT_ALPHA_BLEND_Z]);
        create_pixel_shader(eff_normal_tint_double_mul_z, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NORMAL_TINT_DOUBLE_MUL_Z]);
        create_pixel_shader(eff_normal_tint_mul_z, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NORMAL_TINT_MUL_Z]);
        create_pixel_shader(eff_normal_tint_mul_add_z, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NORMAL_TINT_MUL_ADD_Z]);
        create_pixel_shader(decal_add, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_DECAL_ADD]);
        create_pixel_shader(decal_multiply, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_DECAL_MULTIPLY]);
        create_pixel_shader(decal_multiply2x, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_DECAL_MULTIPLY2X]);
        create_pixel_shader(decal_alpha_blend, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_DECAL_ALPHA_BLEND]);
        create_pixel_shader(decal_alpha_madd, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_DECAL_ALPHA_MULTIPLY_ADD]);

    }

    void rasterizer_release_pixel_shaders() noexcept {
        for(int i = 0; i < NUMBER_OF_CHIMERA_PIXEL_SHADERS; i++) {
            if(chimera_pixel_shaders[i]) {
                IDirect3DPixelShader9_Release(chimera_pixel_shaders[i]);
                chimera_pixel_shaders[i] = nullptr;
            }
        }
    }

    bool rasterizer_compile_shader(const char *source, const char *entry, const char *profile, D3D_SHADER_MACRO *defines, ID3DBlob **compiled_shader) {
        ID3DBlob *error_messages = NULL;
        DWORD flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
        HRESULT result = D3DCompile(source, strlen(source), NULL, defines, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, profile, flags, 0, compiled_shader, &error_messages);
        if(FAILED(result)) {
            if(error_messages != NULL) {
                console_error("Pixel shader failed to compile");
                error_messages->Release();
            }
            else {
                show_error_box("Error", "compiling pixel shader: unknown error\n");
            }
            return false;
        }
        return true;
    }

    bool d3d9_probe_command(int argc, const char **argv) noexcept {
        return D3D9ModelVertexInputDiag::command(argc, argv);
    }

    bool d3d9_transparent_compat_command(int argc, const char **argv) noexcept {
        // A manual per-pass change intentionally leaves automatic hybrid mode so
        // A/B diagnostics are not immediately overwritten on the next EndScene.
        if(argc >= 1
            && (_stricmp(argv[0], "generic_m") == 0
                || _stricmp(argv[0], "plasma_m") == 0
                || _stricmp(argv[0], "plasma") == 0)) {
            validated_hybrid_auto_enabled = false;
            validated_hybrid_announced = false;
        }
        return D3D9TransparentShaderCompat::command(argc, argv);
    }

    bool d3d9_modern_shader_command(int argc, const char **argv) noexcept {
        if(argc >= 1 && _stricmp(argv[0], "hybrid") == 0) {
            if(argc < 2 || _stricmp(argv[1], "status") == 0) {
                console_output(
                    "D3D9On12 validated hybrid: %s (MODEL VS3 + GENERIC_M exact VS2 + PLASMA_M exact VS2).",
                    validated_hybrid_requested() ? "ON" : "OFF"
                );
                return true;
            }
            if(_stricmp(argv[1], "on") == 0) {
                set_validated_hybrid(true);
                return true;
            }
            if(_stricmp(argv[1], "off") == 0) {
                set_validated_hybrid(false);
                return true;
            }
            console_error("D3D9 modern bank: use hybrid on|off|status.");
            return false;
        }

        if(argc >= 1 && _stricmp(argv[0], "model") == 0) {
            // Manual MODEL A/B should remain manual until hybrid is explicitly
            // re-enabled; otherwise EndScene would immediately undo the test.
            validated_hybrid_auto_enabled = false;
            validated_hybrid_announced = false;
        }

        if(argc >= 1 && _stricmp(argv[0], "help") == 0) {
            console_output("chimera_d3d9_modern hybrid on|off|status");
        }
        return D3D9ModernShaderBank::command(argc, argv);
    }

    void set_up_rasterizer() noexcept {
        global_d3d9_device = reinterpret_cast<IDirect3DDevice9 **>(*reinterpret_cast<std::byte **>(get_chimera().get_signature("model_af_set_sampler_states_sig").data() + 1));
        d3d9_device_caps = reinterpret_cast<D3DCAPS9 *>(*reinterpret_cast<std::byte **>(get_chimera().get_signature("d3d9_device_caps_sig").data() + 1));

        // The legacy transparent-geometry fix patches DrawIndexedPrimitive and
        // dynamic index-buffer Lock vtables. Do not stack it with the 9On12
        // live draw probe. Native D3D9 keeps the existing fix unchanged.
        const bool d3d9on12_requested = d3d9on12_requested_now();
        if(!d3d9on12_requested) {
            set_up_environment_transparent_index_buffer_fix();
        }

        // primary_v2 remains installed as the reversible fallback and as the
        // SetVertexShader interception point. In validated hybrid mode its MODEL
        // branch routes the complete MODEL family to the modern VS3 bank.
        set_up_d3d9_model_shader_compat();
        set_up_d3d9_model_shader_primary_v2();
        set_up_d3d9_model_vertex_input_diag();

        add_game_start_event(set_up_d3d9_model_shader_compat);
        add_game_start_event(set_up_d3d9_model_shader_primary_v2);
        add_game_start_event(set_up_d3d9_model_vertex_input_diag);
        add_game_start_event(set_up_validated_hybrid);

        if(d3d9on12_requested) {
            add_d3d9_end_scene_event(ensure_validated_hybrid);
            set_up_validated_hybrid();

            static bool probe_command_registered = false;
            if(!probe_command_registered) {
                get_chimera().get_commands().emplace_back(
                    "chimera_d3d9_probe",
                    "chimera_category_debug",
                    "client",
                    "Live D3D9On12 pass isolation: status/next/reset/toggle/disable/enable/mark",
                    d3d9_probe_command,
                    false,
                    0,
                    2
                );
                probe_command_registered = true;
            }

            static bool transparent_compat_command_registered = false;
            if(!transparent_compat_command_registered) {
                get_chimera().get_commands().emplace_back(
                    "chimera_d3d9_compat",
                    "chimera_category_debug",
                    "client",
                    "Live D3D9On12 transparent shader compatibility: status/generic_m/plasma_m/dump",
                    d3d9_transparent_compat_command,
                    false,
                    0,
                    2
                );
                transparent_compat_command_registered = true;
            }

            static bool modern_shader_command_registered = false;
            if(!modern_shader_command_registered) {
                get_chimera().get_commands().emplace_back(
                    "chimera_d3d9_modern",
                    "chimera_category_debug",
                    "client",
                    "D3D9On12 modern bank: status/dump_all/model/hybrid",
                    d3d9_modern_shader_command,
                    false,
                    0,
                    2
                );
                modern_shader_command_registered = true;
            }
        }

        // Restore any live shader-table replacement before Chimera releases its
        // VS3 shader objects. This keeps Halo's original shader table intact.
        add_game_exit_event(D3D9TransparentShaderCompat::on_game_exit, EVENT_PRIORITY_BEFORE);
        add_game_exit_event(rasterizer_release_vertex_shaders_3_0);
        add_game_exit_event(rasterizer_release_pixel_shaders, EVENT_PRIORITY_AFTER);
        add_game_start_event(rasterizer_create_pixel_shaders);

        chimera_rasterizer_enabled = true;
    }

}
