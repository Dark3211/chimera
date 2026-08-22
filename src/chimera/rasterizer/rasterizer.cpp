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


namespace Chimera {

    IDirect3DDevice9 **global_d3d9_device = nullptr;
    D3DCAPS9 *d3d9_device_caps = nullptr;
    bool chimera_rasterizer_enabled = false;

    IDirect3DPixelShader9 *chimera_pixel_shaders[NUMBER_OF_CHIMERA_PIXEL_SHADERS] = { nullptr };

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
        return D3D9TransparentShaderCompat::command(argc, argv);
    }

    bool d3d9_modern_shader_command(int argc, const char **argv) noexcept {
        return D3D9ModernShaderBank::command(argc, argv);
    }

    void set_up_rasterizer() noexcept {
        global_d3d9_device = reinterpret_cast<IDirect3DDevice9 **>(*reinterpret_cast<std::byte **>(get_chimera().get_signature("model_af_set_sampler_states_sig").data() + 1));
        d3d9_device_caps = reinterpret_cast<D3DCAPS9 *>(*reinterpret_cast<std::byte **>(get_chimera().get_signature("d3d9_device_caps_sig").data() + 1));

        // The legacy transparent-geometry fix patches DrawIndexedPrimitive and
        // dynamic index-buffer Lock vtables. Do not stack it with the 9On12
        // live draw probe. Native D3D9 keeps the existing fix unchanged.
        auto *backend = get_chimera().get_ini()->get_value("video_mode.d3d_backend");
        const bool d3d9on12_requested = backend
            && (_stricmp(backend, "9on12") == 0 || _stricmp(backend, "d3d9on12") == 0);
        if(!d3d9on12_requested) {
            set_up_environment_transparent_index_buffer_fix();
        }

        // Keep only the compatibility path under test plus the live/compact
        // DrawPrimitive/DrawIndexedPrimitive probe. Completed legacy A/B hook
        // stacks remain removed.
        set_up_d3d9_model_shader_compat();
        set_up_d3d9_model_shader_primary_v2();
        set_up_d3d9_model_vertex_input_diag();

        add_game_start_event(set_up_d3d9_model_shader_compat);
        add_game_start_event(set_up_d3d9_model_shader_primary_v2);
        add_game_start_event(set_up_d3d9_model_vertex_input_diag);

        if(d3d9on12_requested) {
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
                    "D3D9On12 modern VS3 bank: status/dump_all",
                    d3d9_modern_shader_command,
                    false,
                    0,
                    1
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
