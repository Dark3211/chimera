// SPDX-License-Identifier: GPL-3.0-only

#include <cctype>
#include <cstdint>
#include <cstring>

#include "rasterizer.hpp"
#include "../chimera.hpp"
#include "../signature/hook.hpp"
#include "../halo_data/game_variables.hpp"
#include "../halo_data/shader_defs.hpp"
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

    extern "C" void *rasterizer_transparent_geometry_group_draw_func;

    namespace {
        enum class EnvironmentTransparentDiagnosticMode {
            NORMAL,
            ENGINE_OFF,
            ALL_CANDIDATES,
            SHADER_TYPE
        };

        static EnvironmentTransparentDiagnosticMode environment_transparent_diagnostic_mode = EnvironmentTransparentDiagnosticMode::NORMAL;
        static short environment_transparent_skipped_shader_type = -1;

        static Hook environment_transparent_group_draw_hook;
        static const void *environment_transparent_group_draw_original = nullptr;

        static bool environment_transparent_engine_flag_snapshot_valid = false;
        static bool environment_transparent_engine_flag_snapshot = true;

        static std::uint64_t environment_transparent_candidate_count = 0;
        static std::uint64_t environment_transparent_shader_counts[NUMBER_OF_SHADER_TYPES] = {};
        static std::uint64_t environment_transparent_other_shader_count = 0;

        static const char *shader_type_name(short shader_type) noexcept {
            switch(shader_type) {
                case SHADER_TYPE_SCREEN:
                    return "screen";
                case SHADER_TYPE_EFFECT:
                    return "effect";
                case SHADER_TYPE_DECAL:
                    return "decal";
                case SHADER_TYPE_ENVIRONMENT:
                    return "environment";
                case SHADER_TYPE_MODEL:
                    return "model";
                case SHADER_TYPE_TRANSPARENT_GENERIC:
                    return "generic";
                case SHADER_TYPE_TRANSPARENT_CHICAGO:
                    return "chicago";
                case SHADER_TYPE_TRANSPARENT_CHICAGO_EXTENDED:
                    return "chicago_extended";
                case SHADER_TYPE_TRANSPARENT_WATER:
                    return "water";
                case SHADER_TYPE_TRANSPARENT_GLASS:
                    return "glass";
                case SHADER_TYPE_TRANSPARENT_METER:
                    return "meter";
                case SHADER_TYPE_TRANSPARENT_PLASMA:
                    return "plasma";
                default:
                    return "other";
            }
        }

        static bool shader_type_from_name(const char *name, std::size_t length, short &shader_type) noexcept {
            if(!name) {
                return false;
            }

            auto matches = [&](const char *value) noexcept {
                return std::strlen(value) == length && std::strncmp(name, value, length) == 0;
            };

            if(matches("screen")) {
                shader_type = SHADER_TYPE_SCREEN;
            }
            else if(matches("effect")) {
                shader_type = SHADER_TYPE_EFFECT;
            }
            else if(matches("decal")) {
                shader_type = SHADER_TYPE_DECAL;
            }
            else if(matches("environment")) {
                shader_type = SHADER_TYPE_ENVIRONMENT;
            }
            else if(matches("model")) {
                shader_type = SHADER_TYPE_MODEL;
            }
            else if(matches("generic")) {
                shader_type = SHADER_TYPE_TRANSPARENT_GENERIC;
            }
            else if(matches("chicago")) {
                shader_type = SHADER_TYPE_TRANSPARENT_CHICAGO;
            }
            else if(matches("chicago_extended")) {
                shader_type = SHADER_TYPE_TRANSPARENT_CHICAGO_EXTENDED;
            }
            else if(matches("water")) {
                shader_type = SHADER_TYPE_TRANSPARENT_WATER;
            }
            else if(matches("glass")) {
                shader_type = SHADER_TYPE_TRANSPARENT_GLASS;
            }
            else if(matches("meter")) {
                shader_type = SHADER_TYPE_TRANSPARENT_METER;
            }
            else if(matches("plasma")) {
                shader_type = SHADER_TYPE_TRANSPARENT_PLASMA;
            }
            else if(matches("other")) {
                shader_type = NUMBER_OF_SHADER_TYPES;
            }
            else {
                return false;
            }

            return true;
        }

        static bool is_environment_transparent_candidate(const TransparentGeometryGroup *group) noexcept {
            if(!group || !group->shader) {
                return false;
            }

            // BSP/environment transparent geometry has no owning object and normally uses
            // static vertex buffers. Keep the diagnostic deliberately narrow so effects,
            // models, HUD geometry, and other transparent paths are not suppressed.
            return group->object_index.is_null()
                && group->source_object_index.is_null()
                && group->dynamic_vertex_buffer_index == -1
                && group->vertex_buffers != nullptr;
        }

        static short get_group_shader_type(const TransparentGeometryGroup *group) noexcept {
            if(!group || !group->shader) {
                return -1;
            }

            return reinterpret_cast<const _shader *>(group->shader)->type;
        }

        static void reset_environment_transparent_stats() noexcept {
            environment_transparent_candidate_count = 0;
            std::memset(environment_transparent_shader_counts, 0, sizeof(environment_transparent_shader_counts));
            environment_transparent_other_shader_count = 0;
        }

        static void restore_environment_transparent_engine_flag() noexcept {
            if(environment_transparent_engine_flag_snapshot_valid && rasterizer_debug_options) {
                rasterizer_debug_options->draw_environment_transparent_geometry = environment_transparent_engine_flag_snapshot;
            }
            environment_transparent_engine_flag_snapshot_valid = false;
        }

        static const char *environment_transparent_mode_name() noexcept {
            switch(environment_transparent_diagnostic_mode) {
                case EnvironmentTransparentDiagnosticMode::ENGINE_OFF:
                    return "engine_off";
                case EnvironmentTransparentDiagnosticMode::ALL_CANDIDATES:
                    return "all";
                case EnvironmentTransparentDiagnosticMode::SHADER_TYPE:
                    return shader_type_name(environment_transparent_skipped_shader_type);
                default:
                    return "normal";
            }
        }

        static void print_environment_transparent_stats() noexcept {
            console_output("chimera_debug_environment_transparent: %s", environment_transparent_mode_name());
            console_output("candidate_groups=%llu other=%llu",
                static_cast<unsigned long long>(environment_transparent_candidate_count),
                static_cast<unsigned long long>(environment_transparent_other_shader_count));

            for(short shader_type = 0; shader_type < NUMBER_OF_SHADER_TYPES; shader_type++) {
                if(environment_transparent_shader_counts[shader_type] != 0) {
                    console_output("%s=%llu",
                        shader_type_name(shader_type),
                        static_cast<unsigned long long>(environment_transparent_shader_counts[shader_type]));
                }
            }
        }

        static void environment_transparent_group_draw_diagnostic(TransparentGeometryGroup *group, bool is_dirty) noexcept {
            using GroupDrawFunction = void (*)(TransparentGeometryGroup *, bool);
            auto original = reinterpret_cast<GroupDrawFunction>(const_cast<void *>(environment_transparent_group_draw_original));
            if(!original) {
                return;
            }

            if(is_environment_transparent_candidate(group)) {
                environment_transparent_candidate_count++;

                short shader_type = get_group_shader_type(group);
                if(shader_type >= 0 && shader_type < NUMBER_OF_SHADER_TYPES) {
                    environment_transparent_shader_counts[shader_type]++;
                }
                else {
                    environment_transparent_other_shader_count++;
                }

                if(environment_transparent_diagnostic_mode == EnvironmentTransparentDiagnosticMode::ALL_CANDIDATES) {
                    return;
                }

                if(environment_transparent_diagnostic_mode == EnvironmentTransparentDiagnosticMode::SHADER_TYPE) {
                    const bool shader_matches = shader_type == environment_transparent_skipped_shader_type;
                    const bool other_matches = environment_transparent_skipped_shader_type == NUMBER_OF_SHADER_TYPES
                        && (shader_type < 0 || shader_type >= NUMBER_OF_SHADER_TYPES);
                    if(shader_matches || other_matches) {
                        return;
                    }
                }
            }

            original(group, is_dirty);
        }

        static bool environment_transparent_diagnostic_command(const char *command) noexcept {
            if(!command) {
                return true;
            }

            static constexpr char command_name[] = "chimera_debug_environment_transparent";
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

            if(*argument == '\0' || std::strcmp(argument, "stats") == 0) {
                print_environment_transparent_stats();
                console_output("modes: normal engine_off all screen effect decal environment model generic chicago chicago_extended water glass meter plasma other reset");
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
                console_error("chimera_debug_environment_transparent: expected one mode");
                return false;
            }

            auto matches = [&](const char *value) noexcept {
                return std::strlen(value) == argument_length && std::strncmp(argument, value, argument_length) == 0;
            };

            if(matches("reset")) {
                reset_environment_transparent_stats();
                console_output("chimera_debug_environment_transparent: counters reset");
                return false;
            }

            restore_environment_transparent_engine_flag();
            environment_transparent_skipped_shader_type = -1;

            if(matches("normal")) {
                environment_transparent_diagnostic_mode = EnvironmentTransparentDiagnosticMode::NORMAL;
            }
            else if(matches("engine_off")) {
                if(!rasterizer_debug_options) {
                    console_error("chimera_debug_environment_transparent: rasterizer debug options unavailable");
                    environment_transparent_diagnostic_mode = EnvironmentTransparentDiagnosticMode::NORMAL;
                    return false;
                }

                environment_transparent_engine_flag_snapshot = rasterizer_debug_options->draw_environment_transparent_geometry;
                environment_transparent_engine_flag_snapshot_valid = true;
                rasterizer_debug_options->draw_environment_transparent_geometry = false;
                environment_transparent_diagnostic_mode = EnvironmentTransparentDiagnosticMode::ENGINE_OFF;
            }
            else if(matches("all")) {
                environment_transparent_diagnostic_mode = EnvironmentTransparentDiagnosticMode::ALL_CANDIDATES;
            }
            else {
                short shader_type = -1;
                if(!shader_type_from_name(argument, argument_length, shader_type)) {
                    console_error("chimera_debug_environment_transparent: unknown mode");
                    console_output("modes: normal engine_off all screen effect decal environment model generic chicago chicago_extended water glass meter plasma other reset");
                    environment_transparent_diagnostic_mode = EnvironmentTransparentDiagnosticMode::NORMAL;
                    return false;
                }

                environment_transparent_skipped_shader_type = shader_type;
                environment_transparent_diagnostic_mode = EnvironmentTransparentDiagnosticMode::SHADER_TYPE;
            }

            reset_environment_transparent_stats();
            console_output("chimera_debug_environment_transparent: %s", environment_transparent_mode_name());
            return false;
        }

        static void set_up_environment_transparent_diagnostic() noexcept {
            auto *target = get_chimera().get_signature("transparent_geometry_group_draw_sig").data();
            if(!target) {
                console_error("chimera_debug_environment_transparent: group draw signature unavailable");
                return;
            }

            write_function_override(
                target,
                environment_transparent_group_draw_hook,
                reinterpret_cast<const void *>(environment_transparent_group_draw_diagnostic),
                &environment_transparent_group_draw_original
            );

            // set_up_function_hooks() runs before set_up_rasterizer(). Point Chimera's
            // assembly thunk at the trampoline so Chimera-internal calls do not recurse
            // back through this diagnostic override.
            if(environment_transparent_group_draw_original) {
                rasterizer_transparent_geometry_group_draw_func = const_cast<void *>(environment_transparent_group_draw_original);
            }
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
        add_command_event(environment_transparent_diagnostic_command, EVENT_PRIORITY_BEFORE);
        set_up_environment_transparent_diagnostic();
        add_game_exit_event(rasterizer_release_vertex_shaders_3_0);
        add_game_exit_event(rasterizer_release_pixel_shaders);
        add_game_start_event(rasterizer_create_pixel_shaders);

        chimera_rasterizer_enabled = true;
    }

}
