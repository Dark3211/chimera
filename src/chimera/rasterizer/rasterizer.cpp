// SPDX-License-Identifier: GPL-3.0-only

#include <cctype>
#include <cstring>

#include "rasterizer.hpp"
#include "../chimera.hpp"
#include "../signature/hook.hpp"
#include "../halo_data/game_variables.hpp"
#include "../halo_data/shaders/shader_blob.hpp"
#include "../output/error_box.hpp"
#include "../output/output.hpp"
#include "../event/command.hpp"
#include "../event/game_loop.hpp"


namespace Chimera {

    IDirect3DDevice9 **global_d3d9_device = nullptr;
    D3DCAPS9 *d3d9_device_caps = nullptr;
    bool chimera_rasterizer_enabled = false;

    IDirect3DPixelShader9 *chimera_pixel_shaders[NUMBER_OF_CHIMERA_PIXEL_SHADERS] = { nullptr };

    namespace {
        enum class DynamicGeometryDiagnosticMode {
            NORMAL,
            UNLIT,
            LIT,
            SCREEN
        };

        struct DynamicGeometryDiagnosticSnapshot {
            bool draw_dynamic_unlit_geometry;
            bool draw_dynamic_lit_geometry;
            bool draw_dynamic_screen_geometry;
        };

        static DynamicGeometryDiagnosticMode dynamic_geometry_diagnostic_mode = DynamicGeometryDiagnosticMode::NORMAL;
        static DynamicGeometryDiagnosticSnapshot dynamic_geometry_diagnostic_snapshot = {};
        static bool dynamic_geometry_diagnostic_snapshot_valid = false;

        static const char *dynamic_geometry_diagnostic_mode_name(DynamicGeometryDiagnosticMode mode) noexcept {
            switch(mode) {
                case DynamicGeometryDiagnosticMode::UNLIT:
                    return "unlit";
                case DynamicGeometryDiagnosticMode::LIT:
                    return "lit";
                case DynamicGeometryDiagnosticMode::SCREEN:
                    return "screen";
                default:
                    return "normal";
            }
        }

        static void capture_dynamic_geometry_diagnostic_snapshot() noexcept {
            if(dynamic_geometry_diagnostic_snapshot_valid || !rasterizer_debug_options) {
                return;
            }

            dynamic_geometry_diagnostic_snapshot.draw_dynamic_unlit_geometry = rasterizer_debug_options->draw_dynamic_unlit_geometry;
            dynamic_geometry_diagnostic_snapshot.draw_dynamic_lit_geometry = rasterizer_debug_options->draw_dynamic_lit_geometry;
            dynamic_geometry_diagnostic_snapshot.draw_dynamic_screen_geometry = rasterizer_debug_options->draw_dynamic_screen_geometry;
            dynamic_geometry_diagnostic_snapshot_valid = true;
        }

        static void restore_dynamic_geometry_diagnostic_snapshot() noexcept {
            if(!dynamic_geometry_diagnostic_snapshot_valid || !rasterizer_debug_options) {
                return;
            }

            rasterizer_debug_options->draw_dynamic_unlit_geometry = dynamic_geometry_diagnostic_snapshot.draw_dynamic_unlit_geometry;
            rasterizer_debug_options->draw_dynamic_lit_geometry = dynamic_geometry_diagnostic_snapshot.draw_dynamic_lit_geometry;
            rasterizer_debug_options->draw_dynamic_screen_geometry = dynamic_geometry_diagnostic_snapshot.draw_dynamic_screen_geometry;
        }

        static bool dynamic_geometry_diagnostic_command(const char *command) noexcept {
            if(!command) {
                return true;
            }

            static constexpr char command_name[] = "chimera_debug_dynamic_geometry";
            static constexpr std::size_t command_name_length = sizeof(command_name) - 1;

            if(std::strncmp(command, command_name, command_name_length) != 0) {
                return true;
            }

            const char *argument = command + command_name_length;
            if(*argument != '\0' && !std::isspace(static_cast<unsigned char>(*argument))) {
                return true;
            }

            while(std::isspace(static_cast<unsigned char>(*argument))) {
                argument++;
            }

            if(*argument == '\0') {
                console_output("chimera_debug_dynamic_geometry: %s", dynamic_geometry_diagnostic_mode_name(dynamic_geometry_diagnostic_mode));
                console_output("modes: normal unlit lit screen");
                return false;
            }

            const char *argument_end = argument;
            while(*argument_end != '\0' && !std::isspace(static_cast<unsigned char>(*argument_end))) {
                argument_end++;
            }

            const std::size_t argument_length = static_cast<std::size_t>(argument_end - argument);
            while(std::isspace(static_cast<unsigned char>(*argument_end))) {
                argument_end++;
            }

            if(*argument_end != '\0') {
                console_error("chimera_debug_dynamic_geometry: expected one mode");
                return false;
            }

            auto matches = [&](const char *value) noexcept {
                return std::strlen(value) == argument_length && std::strncmp(argument, value, argument_length) == 0;
            };

            DynamicGeometryDiagnosticMode new_mode;
            if(matches("normal")) {
                new_mode = DynamicGeometryDiagnosticMode::NORMAL;
            }
            else if(matches("unlit")) {
                new_mode = DynamicGeometryDiagnosticMode::UNLIT;
            }
            else if(matches("lit")) {
                new_mode = DynamicGeometryDiagnosticMode::LIT;
            }
            else if(matches("screen")) {
                new_mode = DynamicGeometryDiagnosticMode::SCREEN;
            }
            else {
                console_error("chimera_debug_dynamic_geometry: unknown mode");
                console_output("modes: normal unlit lit screen");
                return false;
            }

            if(!rasterizer_debug_options) {
                console_error("chimera_debug_dynamic_geometry: rasterizer debug options unavailable");
                return false;
            }

            capture_dynamic_geometry_diagnostic_snapshot();
            restore_dynamic_geometry_diagnostic_snapshot();

            switch(new_mode) {
                case DynamicGeometryDiagnosticMode::UNLIT:
                    rasterizer_debug_options->draw_dynamic_unlit_geometry = false;
                    break;
                case DynamicGeometryDiagnosticMode::LIT:
                    rasterizer_debug_options->draw_dynamic_lit_geometry = false;
                    break;
                case DynamicGeometryDiagnosticMode::SCREEN:
                    rasterizer_debug_options->draw_dynamic_screen_geometry = false;
                    break;
                default:
                    dynamic_geometry_diagnostic_snapshot_valid = false;
                    break;
            }

            dynamic_geometry_diagnostic_mode = new_mode;
            console_output("chimera_debug_dynamic_geometry: %s", dynamic_geometry_diagnostic_mode_name(dynamic_geometry_diagnostic_mode));
            return false;
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

        IDirect3DDevice9_CreatePixelShader(*global_d3d9_device, reinterpret_cast<DWORD *>(white_1_1), &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_WHITE_1_1]);

        // Ensure ps2.0 support for all except the ps_1_1 shader.
        if(d3d9_device_caps->PixelShaderVersion < 0xffff0200) {
            return;
        }

        IDirect3DDevice9_CreatePixelShader(*global_d3d9_device, reinterpret_cast<DWORD *>(white), &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_WHITE]);
        IDirect3DDevice9_CreatePixelShader(*global_d3d9_device, reinterpret_cast<DWORD *>(black), &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_BLACK]);
        IDirect3DDevice9_CreatePixelShader(*global_d3d9_device, reinterpret_cast<DWORD *>(hud_meters), &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_HUD_METERS]);
        IDirect3DDevice9_CreatePixelShader(*global_d3d9_device, reinterpret_cast<DWORD *>(fog), &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_FOG]);
        IDirect3DDevice9_CreatePixelShader(*global_d3d9_device, reinterpret_cast<DWORD *>(fog_akill), &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_FOG_ALPHA_KILL]);
        IDirect3DDevice9_CreatePixelShader(*global_d3d9_device, reinterpret_cast<DWORD *>(fog_screen), &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_FOG_SCREEN]);
        IDirect3DDevice9_CreatePixelShader(*global_d3d9_device, reinterpret_cast<DWORD *>(eff_nlin_tint_z), &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NLIN_TINT_Z]);
        IDirect3DDevice9_CreatePixelShader(*global_d3d9_device, reinterpret_cast<DWORD *>(eff_nlin_tint_add_z), &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NLIN_TINT_ADD_Z]);
        IDirect3DDevice9_CreatePixelShader(*global_d3d9_device, reinterpret_cast<DWORD *>(eff_nlin_tint_alpha_blend_z), &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NLIN_TINT_ALPHA_BLEND_Z]);
        IDirect3DDevice9_CreatePixelShader(*global_d3d9_device, reinterpret_cast<DWORD *>(eff_nlin_tint_double_mul_z), &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NLIN_TINT_DOUBLE_MUL_Z]);
        IDirect3DDevice9_CreatePixelShader(*global_d3d9_device, reinterpret_cast<DWORD *>(eff_nlin_tint_mul_z), &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NLIN_TINT_MUL_Z]);
        IDirect3DDevice9_CreatePixelShader(*global_d3d9_device, reinterpret_cast<DWORD *>(eff_nlin_tint_mul_add_z), &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NLIN_TINT_MUL_ADD_Z]);
        IDirect3DDevice9_CreatePixelShader(*global_d3d9_device, reinterpret_cast<DWORD *>(eff_normal_tint_z), &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NORMAL_TINT_Z]);
        IDirect3DDevice9_CreatePixelShader(*global_d3d9_device, reinterpret_cast<DWORD *>(eff_normal_tint_add_z), &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NORMAL_TINT_ADD_Z]);
        IDirect3DDevice9_CreatePixelShader(*global_d3d9_device, reinterpret_cast<DWORD *>(eff_normal_tint_alpha_blend_z), &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NORMAL_TINT_ALPHA_BLEND_Z]);
        IDirect3DDevice9_CreatePixelShader(*global_d3d9_device, reinterpret_cast<DWORD *>(eff_normal_tint_double_mul_z), &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NORMAL_TINT_DOUBLE_MUL_Z]);
        IDirect3DDevice9_CreatePixelShader(*global_d3d9_device, reinterpret_cast<DWORD *>(eff_normal_tint_mul_z), &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NORMAL_TINT_MUL_Z]);
        IDirect3DDevice9_CreatePixelShader(*global_d3d9_device, reinterpret_cast<DWORD *>(eff_normal_tint_mul_add_z), &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NORMAL_TINT_MUL_ADD_Z]);
        IDirect3DDevice9_CreatePixelShader(*global_d3d9_device, reinterpret_cast<DWORD *>(decal_add), &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_DECAL_ADD]);
        IDirect3DDevice9_CreatePixelShader(*global_d3d9_device, reinterpret_cast<DWORD *>(decal_multiply), &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_DECAL_MULTIPLY]);
        IDirect3DDevice9_CreatePixelShader(*global_d3d9_device, reinterpret_cast<DWORD *>(decal_multiply2x), &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_DECAL_MULTIPLY2X]);
        IDirect3DDevice9_CreatePixelShader(*global_d3d9_device, reinterpret_cast<DWORD *>(decal_alpha_blend), &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_DECAL_ALPHA_BLEND]);
        IDirect3DDevice9_CreatePixelShader(*global_d3d9_device, reinterpret_cast<DWORD *>(decal_alpha_madd), &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_DECAL_ALPHA_MULTIPLY_ADD]);

    }

    void rasterizer_release_pixel_shaders() noexcept {
        for(int i = 0; i < NUMBER_OF_CHIMERA_PIXEL_SHADERS; i++) {
            if(chimera_pixel_shaders[i]) {
                IDirect3DPixelShader9_Release(chimera_pixel_shaders[i]);
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

    void set_up_rasterizer() noexcept {
        global_d3d9_device = reinterpret_cast<IDirect3DDevice9 **>(*reinterpret_cast<std::byte **>(get_chimera().get_signature("model_af_set_sampler_states_sig").data() + 1));
        d3d9_device_caps = reinterpret_cast<D3DCAPS9 *>(*reinterpret_cast<std::byte **>(get_chimera().get_signature("d3d9_device_caps_sig").data() + 1));
        add_command_event(dynamic_geometry_diagnostic_command, EVENT_PRIORITY_BEFORE);
        add_game_exit_event(rasterizer_release_vertex_shaders_3_0);
        add_game_exit_event(rasterizer_release_pixel_shaders);
        add_game_start_event(rasterizer_create_pixel_shaders);

        chimera_rasterizer_enabled = true;
    }

}
