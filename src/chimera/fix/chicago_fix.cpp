// SPDX-License-Identifier: GPL-3.0-only

#include <cstring>

#include "chicago_fix.hpp"
#include "map_hacks/map_hacks.hpp"
#include "../chimera.hpp"
#include "../signature/signature.hpp"
#include "../signature/hook.hpp"
#include "../rasterizer/rasterizer.hpp"
#include "../halo_data/shader_defs.hpp"
#include "../halo_data/rasterizer_common.hpp"
#include "../halo_data/game_variables.hpp"
#include "../halo_data/game_functions.hpp"
#include "../rasterizer/rasterizer_vertex_shaders.hpp"
#include "../rasterizer/rasterizer_transparent_geometry.hpp"
#include "../math_trig/math_trig.hpp"


namespace Chimera {

    extern "C" {
        void chicago_multiply_set_xbox_blending_asm() noexcept;
        void chicago_multiply2x_set_xbox_blending_asm() noexcept;
        void chicago_extended_multiply_set_xbox_blending_asm() noexcept;
        void chicago_extended_multiply2x_set_xbox_blending_asm() noexcept;

        void chicago_set_projective_divide_asm() noexcept;
        void chicago_set_up_pixel_shader_asm() noexcept;
        void chicago_extended_set_up_pixel_shader_asm() noexcept;
        const void *chicago_draw_original_ptr = nullptr;
        const void *chicago_extended_draw_original_ptr = nullptr;
    }

    extern "C" void chicago_multiply_set_xbox_blending(_shader *shader, std::uint32_t stage, DWORD tss_option_argument) noexcept {
        if(global_fix_flags.gearbox_chicago_multiply) {
            return;
        }

        bool extended = shader->type == SHADER_TYPE_TRANSPARENT_CHICAGO_EXTENDED;
        if(extended) {
            ShaderTransparentChicagoExtended *shader_data = reinterpret_cast<ShaderTransparentChicagoExtended *>(shader);
            if(TEST_FLAG(shader_data->chicago_extended.extra_flags, SHADER_TRANSPARENT_CHICAGO_EXTRA_FLAGS_CUSTOM_EDITION_BLENDING) || !chimera_rasterizer_enabled) {
                return;
            }
        }
        else {
            ShaderTransparentChicago *shader_data = reinterpret_cast<ShaderTransparentChicago *>(shader);
            if(TEST_FLAG(shader_data->chicago.extra_flags, SHADER_TRANSPARENT_CHICAGO_EXTRA_FLAGS_CUSTOM_EDITION_BLENDING) || !chimera_rasterizer_enabled) {
                return;
            }
        }
        IDirect3DDevice9_SetTextureStageState(*global_d3d9_device, stage, D3DTSS_COLORARG1, D3DTA_CURRENT);
        IDirect3DDevice9_SetTextureStageState(*global_d3d9_device, stage, D3DTSS_COLORARG2, tss_option_argument);
        IDirect3DDevice9_SetTextureStageState(*global_d3d9_device, stage, D3DTSS_COLORARG0, tss_option_argument | D3DTA_COMPLEMENT);
    }

    extern "C" void chicago_multiply2x_set_xbox_blending(_shader *shader, std::uint32_t stage, DWORD tss_option_argument) noexcept {
        if(global_fix_flags.gearbox_chicago_multiply) {
            return;
        }

        bool extended = shader->type == SHADER_TYPE_TRANSPARENT_CHICAGO_EXTENDED;
        if(extended) {
            ShaderTransparentChicagoExtended *shader_data = reinterpret_cast<ShaderTransparentChicagoExtended *>(shader);
            if(TEST_FLAG(shader_data->chicago_extended.extra_flags, SHADER_TRANSPARENT_CHICAGO_EXTRA_FLAGS_CUSTOM_EDITION_BLENDING) || !chimera_rasterizer_enabled) {
                return;
            }
        }
        else {
            ShaderTransparentChicago *shader_data = reinterpret_cast<ShaderTransparentChicago *>(shader);
            if(TEST_FLAG(shader_data->chicago.extra_flags, SHADER_TRANSPARENT_CHICAGO_EXTRA_FLAGS_CUSTOM_EDITION_BLENDING) || !chimera_rasterizer_enabled) {
                return;
            }
        }
        IDirect3DDevice9_SetTextureStageState(*global_d3d9_device, stage, D3DTSS_COLORARG0, tss_option_argument);
        IDirect3DDevice9_SetTextureStageState(*global_d3d9_device, stage, D3DTSS_COLORARG1, D3DTA_CURRENT);
        IDirect3DDevice9_SetTextureStageState(*global_d3d9_device, stage, D3DTSS_COLORARG2, D3DTA_TFACTOR);
    }

    void chicago_fog_fix(TransparentGeometryGroup *group) noexcept {
        if(!group || !global_d3d9_device || !*global_d3d9_device ||
           !d3d9_device_caps || !global_window_parameters) {
            return;
        }
        // Test for the literal potatos that are the reason this is broken in the first place.
        if(d3d9_device_caps->MaxTextureBlendStages < 5 || d3d9_device_caps->MaxSimultaneousTextures < 3 || !(d3d9_device_caps->PrimitiveMiscCaps & D3DPMISCCAPS_PERSTAGECONSTANT)) {
            return;
        }

        if(!rasterizer_debug_options->draw_environment_fog) {
            return;
        }

        _shader *shader = reinterpret_cast<_shader *>(group->shader);
        std::uint16_t framebuffer_blend_function = 0;
        std::uint32_t stage = 0;

        if(shader->type == SHADER_TYPE_TRANSPARENT_CHICAGO) {
            ShaderTransparentChicago *chicago_shader = reinterpret_cast<ShaderTransparentChicago *>(group->shader);
            framebuffer_blend_function = chicago_shader->chicago.framebuffer_blend_function;
            stage = chicago_shader->chicago.maps.count;
        }
        else if(shader->type == SHADER_TYPE_TRANSPARENT_CHICAGO_EXTENDED) {
            ShaderTransparentChicagoExtended *extended_shader = reinterpret_cast<ShaderTransparentChicagoExtended *>(group->shader);
            framebuffer_blend_function = extended_shader->chicago_extended.framebuffer_blend_function;
            stage = extended_shader->chicago_extended.maps.count;
        }

        if(TEST_FLAG(group->geometry_flags, RASTERIZER_GEOMETRY_FLAGS_SKY_BIT) && framebuffer_blend_function == SHADER_FRAMEBUFFER_BLEND_FUNCTION_ALPHA_BLEND) {
            Plane3D *plane = &global_window_parameters->fog.plane;
            Point3D *camera = &global_window_parameters->camera.position;

            float eye_distance_to_fog_plane = plane->i * camera->x + plane->j * camera->y + plane->k * camera->z - plane->w;
            float planar_eye_density = global_window_parameters->fog.planar_maximum_density * PIN(-eye_distance_to_fog_plane / global_window_parameters->fog.planar_maximum_depth, 0.0f, 1.0f);

            ColorARGB color = { planar_eye_density, global_window_parameters->fog.planar_color.red, global_window_parameters->fog.planar_color.green, global_window_parameters->fog.planar_color.blue };

            // Interpolate between current stage color and the fog plane color by the planar eye density.
            IDirect3DDevice9_SetTextureStageState(*global_d3d9_device, stage, D3DTSS_CONSTANT, real_argb_color_to_pixel32(&color));
            IDirect3DDevice9_SetTextureStageState(*global_d3d9_device, stage, D3DTSS_COLORARG2, D3DTA_CURRENT);
            IDirect3DDevice9_SetTextureStageState(*global_d3d9_device, stage, D3DTSS_COLORARG1, D3DTA_CONSTANT);
            IDirect3DDevice9_SetTextureStageState(*global_d3d9_device, stage, D3DTSS_COLORARG0, D3DTA_CONSTANT | D3DTA_ALPHAREPLICATE);
            IDirect3DDevice9_SetTextureStageState(*global_d3d9_device, stage, D3DTSS_COLOROP, D3DTOP_LERP);
            IDirect3DDevice9_SetTextureStageState(*global_d3d9_device, stage, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            IDirect3DDevice9_SetTextureStageState(*global_d3d9_device, stage, D3DTSS_ALPHAARG1, D3DTA_CURRENT);

            IDirect3DDevice9_SetTextureStageState(*global_d3d9_device, stage + 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            IDirect3DDevice9_SetTextureStageState(*global_d3d9_device, stage + 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        }
    }

    extern "C" void chicago_projective_divide_first_map(TransparentGeometryGroup *group) noexcept {
        if(!group || !group->shader || !global_d3d9_device || !*global_d3d9_device ||
           !is_readable_memory_range(group->shader, sizeof(_shader))) {
            return;
        }
        _shader *shader = reinterpret_cast<_shader *>(group->shader);
        // We only want to enable projective divide on stage 0 if the first map is 2D
        bool first_map_is_cube = false;
        if(shader->type == SHADER_TYPE_TRANSPARENT_CHICAGO) {
            ShaderTransparentChicago *chicago_shader = reinterpret_cast<ShaderTransparentChicago *>(group->shader);
            first_map_is_cube = chicago_shader->chicago.type > SHADER_TRANSPARENT_CHICAGO_TYPE_2D_MAP ? true : false;
        }
        else if(shader->type == SHADER_TYPE_TRANSPARENT_CHICAGO_EXTENDED) {
            ShaderTransparentChicagoExtended *extended_shader = reinterpret_cast<ShaderTransparentChicagoExtended *>(group->shader);
            first_map_is_cube = extended_shader->chicago_extended.type > SHADER_TRANSPARENT_CHICAGO_TYPE_2D_MAP ? true : false;
        }
        else {
            // Something seriously borkus must have happened for this to ever happen
            return;
        }

        if(!first_map_is_cube) {
            IDirect3DDevice9_SetTextureStageState(*global_d3d9_device, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_PROJECTED);
        }

        // Extra Chicago layers can change the vertex shader. Restore the shader and
        // declaration required by the parent geometry group before the final draw.
        const short vertex_shader_permutation = shader_get_vertex_shader_permutation(reinterpret_cast<std::byte *>(group->shader));
        const short vertex_type = rasterizer_transparent_geometry_get_primary_vertex_type(group);
        if(vertex_shader_permutation >= 0 && vertex_type >= 0) {
            auto *vertex_shader = rasterizer_get_vertex_shader_for_permutation(static_cast<std::uint16_t>(vertex_shader_permutation), vertex_type);
            auto *vertex_declaration = rasterizer_get_vertex_declaration(vertex_type);
            if(vertex_shader && vertex_declaration) {
                IDirect3DDevice9_SetVertexShader(*global_d3d9_device, vertex_shader);
                IDirect3DDevice9_SetVertexDeclaration(*global_d3d9_device, vertex_declaration);
            }
        }

        // Fix fog blending when the sky bit is set and the framebuffer blend function is alpha blend.
        chicago_fog_fix(group);
    }

    void reset_texture_transform_flags() noexcept {
        if(global_d3d9_device && *global_d3d9_device) {
            IDirect3DDevice9_SetTextureStageState(*global_d3d9_device, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        }
    }

    void set_up_chicago_fix() noexcept {
        // Install the optional SM3 backend via Halo's dispatch table. Validate the
        // table and targets before patching. The assembly trampoline returns to the
        // original Gearbox draw whenever the renderer returns false.
        if(chimera_rasterizer_enabled) {
            auto *switch_signature = get_chimera().get_signature("transparent_shader_draw_switch_sig").data();
            if(switch_signature && is_readable_memory_range(switch_signature + 3, sizeof(std::byte *))) {
                std::byte *dispatch_table = nullptr;
                std::memcpy(&dispatch_table, switch_signature + 3, sizeof(dispatch_table));
                if(dispatch_table && is_readable_memory_range(dispatch_table, 7 * sizeof(void *))) {
                    std::byte *chicago_entry = nullptr;
                    std::byte *extended_entry = nullptr;
                    std::memcpy(&chicago_entry, dispatch_table + 5 * sizeof(void *), sizeof(chicago_entry));
                    std::memcpy(&extended_entry, dispatch_table + 6 * sizeof(void *), sizeof(extended_entry));
                    static Hook chicago_draw_hook, extended_draw_hook;
                    if(chicago_entry && is_executable_memory_range(chicago_entry + 5, 16)) {
                        write_function_override(chicago_entry + 5, chicago_draw_hook,
                            reinterpret_cast<const void *>(chicago_set_up_pixel_shader_asm),
                            &chicago_draw_original_ptr);
                        if(!chicago_draw_hook.address) chicago_draw_original_ptr = nullptr;
                    }
                    if(extended_entry && is_executable_memory_range(extended_entry + 5, 16)) {
                        write_function_override(extended_entry + 5, extended_draw_hook,
                            reinterpret_cast<const void *>(chicago_extended_set_up_pixel_shader_asm),
                            &chicago_extended_draw_original_ptr);
                        if(!extended_draw_hook.address) chicago_extended_draw_original_ptr = nullptr;
                    }
                }
            }
        }

        // Keep the original multiply and projective-divide corrections for the
        // fixed-function fallback when SM3 is unavailable or shader creation fails.
        // Fix multiply and multiply2x blend functions.
        // We'll check the gearbox blending flag added in H1A for if we want borked rendering for some tags.
        static Hook multiply, multiply2x, extended_multiply, extended_multiply2x;
        write_jmp_call(get_chimera().get_signature("chicago_multiply_sig").data() + 31, multiply, nullptr, reinterpret_cast<const void *>(chicago_multiply_set_xbox_blending_asm));
        write_jmp_call(get_chimera().get_signature("chicago_multiply2x_sig").data() + 32, multiply2x, nullptr, reinterpret_cast<const void *>(chicago_multiply2x_set_xbox_blending_asm));
        write_jmp_call(get_chimera().get_signature("chicago_extended_multiply_sig").data() + 31, extended_multiply, nullptr, reinterpret_cast<const void *>(chicago_extended_multiply_set_xbox_blending_asm));
        write_jmp_call(get_chimera().get_signature("chicago_extended_multiply2x_sig").data() + 32, extended_multiply2x, nullptr, reinterpret_cast<const void *>(chicago_extended_multiply2x_set_xbox_blending_asm));

        // Fix shader not performing perspetive division when sampling the first map (if it's 2D). Updated vetex shaders required for it to work properly.
        static Hook projective_divide, projective_divide_extended;
        write_jmp_call(get_chimera().get_signature("chicago_group_draw_function_sig").data(), projective_divide, reinterpret_cast<const void *>(chicago_set_projective_divide_asm), reinterpret_cast<const void *>(reset_texture_transform_flags));
        write_jmp_call(get_chimera().get_signature("chicago_extended_group_draw_function_sig").data(), projective_divide_extended, reinterpret_cast<const void *>(chicago_set_projective_divide_asm), reinterpret_cast<const void *>(reset_texture_transform_flags));
    }
}
