// SPDX-License-Identifier: GPL-3.0-only

#include <cstring>
#include <vector>

#include "rasterizer.hpp"
#include "rasterizer_transparent_geometry.hpp"
#include "d3d9_model_shader_compat.hpp"
#include "d3d9_model_shader_primary_v2.hpp"
#include "d3d9_model_vertex_input_diag.hpp"
#include "d3d9_transparent_shader_compat.hpp"
#include "d3d9_modern_shader_bank.hpp"
#include "d3d9_vsh_enc_export.hpp"
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

    static bool d3d9_hybrid_default_requested() noexcept {
        auto *backend = get_chimera().get_ini()->get_value("video_mode.d3d_backend");
        if(!backend
            || (_stricmp(backend, "9on12") != 0 && _stricmp(backend, "d3d9on12") != 0)) {
            return false;
        }

        // Bind-time hybrid routing is the D3D9On12 default. These values retain
        // a reversible escape hatch without changing Halo's stock shader table.
        auto *mode = get_chimera().get_ini()->get_value("video_mode.d3d_shader_backend");
        if(!mode || !*mode) {
            return true;
        }
        if(_stricmp(mode, "off") == 0
            || _stricmp(mode, "stock") == 0
            || _stricmp(mode, "legacy") == 0
            || _stricmp(mode, "manual") == 0) {
            return false;
        }
        return true;
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

    static bool same_vertex_shader_program(
        IDirect3DVertexShader9 *first,
        IDirect3DVertexShader9 *second
    ) noexcept {
        if(first == second) {
            return first != nullptr;
        }
        if(!first || !second) {
            return false;
        }

        UINT first_size = 0;
        UINT second_size = 0;
        if(FAILED(first->GetFunction(nullptr, &first_size))
            || FAILED(second->GetFunction(nullptr, &second_size))
            || first_size == 0
            || first_size != second_size) {
            return false;
        }

        std::vector<unsigned char> first_code(first_size);
        std::vector<unsigned char> second_code(second_size);
        if(FAILED(first->GetFunction(first_code.data(), &first_size))
            || FAILED(second->GetFunction(second_code.data(), &second_size))
            || first_size != second_size) {
            return false;
        }

        return std::memcmp(first_code.data(), second_code.data(), first_size) == 0;
    }

    static bool sync_generic_m_generation(bool report_error = true) noexcept {
        using namespace D3D9TransparentShaderCompat;
        if(!vertex_shaders) {
            return false;
        }

        IDirect3DVertexShader9 *current = vertex_shaders[VSH_TRANSPARENT_GENERIC_M].shader;
        if(!current) {
            return false;
        }

        if(exact_generic_m && current == exact_generic_m) {
            generic_mode = GENERIC_MODE_EXACT_VS2;
            return true;
        }
        if(legacy_generic_m && current == legacy_generic_m) {
            generic_mode = GENERIC_MODE_LEGACY_VS3;
            return true;
        }

        if(!stock_generic_m) {
            stock_generic_m = current;
            stock_generic_m->AddRef();
            generic_mode = GENERIC_MODE_STOCK;
            return true;
        }

        if(current == stock_generic_m) {
            generic_mode = GENERIC_MODE_STOCK;
            return true;
        }

        // Halo rebuilds the shader table when maps change. Accept a new COM
        // object only if its bytecode is exactly the same as the previously
        // captured stock shader. This distinguishes a new stock generation from
        // an unrelated external replacement and preserves the safety check.
        if(same_vertex_shader_program(current, stock_generic_m)) {
            stock_generic_m->Release();
            stock_generic_m = current;
            stock_generic_m->AddRef();
            generic_mode = GENERIC_MODE_STOCK;
            console_output("D3D9 transparent compat: GENERIC_M stock slot refreshed after map change.");
            return true;
        }

        if(report_error) {
            console_error("D3D9 transparent compat: GENERIC_M slot differs from stock/compat; leaving it untouched.");
        }
        return false;
    }

    static bool sync_plasma_m_generation(bool report_error = true) noexcept {
        using namespace D3D9TransparentShaderCompat;
        if(!vertex_shaders) {
            return false;
        }

        IDirect3DVertexShader9 *current = vertex_shaders[VSH_TRANSPARENT_PLASMA_M].shader;
        if(!current) {
            return false;
        }

        if(exact_plasma_m && current == exact_plasma_m) {
            plasma_m_active = true;
            return true;
        }

        if(!stock_plasma_m) {
            stock_plasma_m = current;
            stock_plasma_m->AddRef();
            plasma_m_active = false;
            return true;
        }

        if(current == stock_plasma_m) {
            plasma_m_active = false;
            return true;
        }

        if(same_vertex_shader_program(current, stock_plasma_m)) {
            stock_plasma_m->Release();
            stock_plasma_m = current;
            stock_plasma_m->AddRef();
            plasma_m_active = false;
            console_output("D3D9 transparent compat: PLASMA_M stock slot refreshed after map change.");
            return true;
        }

        if(report_error) {
            console_error("D3D9 transparent compat: PLASMA_M slot differs from stock/compat; leaving it untouched.");
        }
        return false;
    }

    bool d3d9_transparent_compat_command(int argc, const char **argv) noexcept {
        if(argc == 0 || _stricmp(argv[0], "status") == 0) {
            sync_generic_m_generation(false);
            sync_plasma_m_generation(false);
        }
        else if(_stricmp(argv[0], "generic_m") == 0) {
            if(!sync_generic_m_generation()) {
                return false;
            }
        }
        else if(_stricmp(argv[0], "plasma_m") == 0 || _stricmp(argv[0], "plasma") == 0) {
            if(!sync_plasma_m_generation()) {
                return false;
            }
        }
        else if(_stricmp(argv[0], "dump") == 0) {
            if(!sync_generic_m_generation() || !sync_plasma_m_generation()) {
                return false;
            }
        }

        return D3D9TransparentShaderCompat::command(argc, argv);
    }

    static bool d3d9_hybrid_enable() noexcept {
        // Do not replace Halo's stock vertex_shaders[] entries. Build the
        // validated alternatives, then let SetVertexShader route each stock
        // object to the correct D3D9On12 shader at bind time.
        sync_generic_m_generation(false);
        sync_plasma_m_generation(false);

        if(D3D9TransparentShaderCompat::generic_mode != D3D9TransparentShaderCompat::GENERIC_MODE_STOCK) {
            D3D9TransparentShaderCompat::restore_generic_m();
        }
        if(D3D9TransparentShaderCompat::plasma_m_active) {
            D3D9TransparentShaderCompat::restore_plasma_m();
        }

        if(!D3D9ModernShaderBank::model_family_ready()) {
            console_error("D3D9 hybrid: MODEL VS3 family is not ready; inspect chimera_d3d9_modern_build.log.");
            return false;
        }
        if(!D3D9TransparentShaderCompat::prepare_exact_shaders()) {
            console_error("D3D9 hybrid: exact GENERIC_M/PLASMA_M VS2 shaders are not ready.");
            return false;
        }

        D3D9ModelShaderPrimaryV2::set_hybrid_bind_routing(true);
        rasterizer_set_modern_model_test(true);
        console_output("D3D9 hybrid: ON (bind routing; MODEL=VS3, GENERIC_M=exact VS2, PLASMA_M=exact VS2; stock table untouched).");
        return true;
    }

    static bool d3d9_hybrid_disable() noexcept {
        D3D9ModelShaderPrimaryV2::set_hybrid_bind_routing(false);
        rasterizer_set_modern_model_test(false);

        // Also clean up a direct diagnostic replacement if one was enabled
        // manually through chimera_d3d9_compat.
        sync_generic_m_generation(false);
        sync_plasma_m_generation(false);
        bool ok = true;
        if(D3D9TransparentShaderCompat::generic_mode != D3D9TransparentShaderCompat::GENERIC_MODE_STOCK) {
            ok = D3D9TransparentShaderCompat::restore_generic_m() && ok;
        }
        if(D3D9TransparentShaderCompat::plasma_m_active) {
            ok = D3D9TransparentShaderCompat::restore_plasma_m() && ok;
        }

        if(ok) {
            console_output("D3D9 hybrid: OFF (bind routing disabled; primary_v2/current fallback path active).");
        }
        else {
            console_error("D3D9 hybrid: bind routing disabled, but an unknown diagnostic slot was left untouched.");
        }
        return ok;
    }

    static void d3d9_hybrid_status() noexcept {
        sync_generic_m_generation(false);
        sync_plasma_m_generation(false);

        const bool bind_route = D3D9ModelShaderPrimaryV2::hybrid_bind_routing_enabled();
        const bool model = bind_route || rasterizer_modern_model_test_enabled();
        const bool generic = bind_route
            || D3D9TransparentShaderCompat::generic_mode == D3D9TransparentShaderCompat::GENERIC_MODE_EXACT_VS2;
        const bool plasma = bind_route || D3D9TransparentShaderCompat::plasma_m_active;
        console_output(
            "D3D9 hybrid status: MODEL=%s GENERIC_M=%s PLASMA_M=%s BIND_ROUTE=%s DEFAULT=%s",
            model ? "VS3" : "fallback",
            generic ? "exact-vs2" : "stock/other",
            plasma ? "exact-vs2" : "stock",
            bind_route ? "ON" : "OFF",
            d3d9_hybrid_default_requested() ? "ON" : "OFF"
        );
    }

    bool d3d9_modern_shader_command(int argc, const char **argv) noexcept {
        if(argc >= 1
            && (_stricmp(argv[0], "export_vsh") == 0
                || _stricmp(argv[0], "export_vsh_9on12") == 0)) {
            return D3D9VshEncExport::export_vsh_9on12();
        }

        if(argc >= 1 && _stricmp(argv[0], "hybrid") == 0) {
            if(argc < 2 || _stricmp(argv[1], "status") == 0) {
                d3d9_hybrid_status();
                return true;
            }
            if(_stricmp(argv[1], "on") == 0) {
                return d3d9_hybrid_enable();
            }
            if(_stricmp(argv[1], "off") == 0) {
                return d3d9_hybrid_disable();
            }
            console_error("D3D9 modern bank: use hybrid on|off|status.");
            return false;
        }

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

        set_up_d3d9_model_shader_compat();
        set_up_d3d9_model_shader_primary_v2();
        set_up_d3d9_model_vertex_input_diag();

        add_game_start_event(set_up_d3d9_model_shader_compat);
        add_game_start_event(set_up_d3d9_model_shader_primary_v2);
        add_game_start_event(set_up_d3d9_model_vertex_input_diag);

        if(d3d9on12_requested) {
            // Enable the validated hybrid before the first render call. Unlike
            // the old startup experiment this does not write vertex_shaders[];
            // SetVertexShader performs a temporary bind-time substitution only.
            const bool startup_hybrid = d3d9_hybrid_default_requested();
            D3D9ModelShaderPrimaryV2::set_hybrid_bind_routing(startup_hybrid);
            rasterizer_set_modern_model_test(startup_hybrid);
            if(startup_hybrid) {
                console_output("D3D9 hybrid: startup bind routing armed (stock shader table untouched).");
            }

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
                    "D3D9On12 modern VS3 bank: status/dump_all/model/hybrid/export_vsh",
                    d3d9_modern_shader_command,
                    false,
                    0,
                    2
                );
                modern_shader_command_registered = true;
            }
        }

        // Exact/modern shader objects may be recreated between Halo game loops,
        // but bind routing stays armed. The first subsequent stock bind rebuilds
        // any released candidate before forwarding it to the device.
        add_game_exit_event(D3D9TransparentShaderCompat::on_game_exit, EVENT_PRIORITY_BEFORE);
        add_game_exit_event(rasterizer_release_vertex_shaders_3_0);
        add_game_exit_event(rasterizer_release_pixel_shaders, EVENT_PRIORITY_AFTER);
        add_game_start_event(rasterizer_create_pixel_shaders);

        chimera_rasterizer_enabled = true;
    }

}
