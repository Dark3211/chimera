// SPDX-License-Identifier: GPL-3.0-only

#include "effect_shader_fix.hpp"
#include "../chimera.hpp"
#include "../signature/hook.hpp"
#include "../signature/signature.hpp"
#include "../halo_data/bitmaps.hpp"
#include "../halo_data/game_variables.hpp"
#include "../halo_data/game_functions.hpp"
#include "../halo_data/shader_defs.hpp"
#include "../halo_data/shader_effects.hpp"
#include "../halo_data/game_engine.hpp"
#include "../math_trig/math_trig.hpp"
#include "../rasterizer/rasterizer.hpp"
#include "../rasterizer/rasterizer_vertex_shaders.hpp"


namespace Chimera {

    #define RASTERIZER_TRANSPARENT_GEOMETRY_TEXCOORD_STREAM_SIZE 8192;

    extern "C" {
        void effect_shader_reindex_pixel_shader_asm() noexcept;
        const void *effect_shader_get_ps_original = nullptr;
        std::uint32_t effect_shader_permutation_index;

        void shader_set_up_zprite_retail_asm() noexcept;
        const void *shader_effect_d3dx_begin = nullptr;
        const void *shader_effect_draw_end = nullptr;
        bool zsprite_drawn = false;

        void effect_shader_set_up_zsprite_custom_asm() noexcept;
    }
    
    IDirect3DVertexBuffer9 **aux_buffer = nullptr;

    extern "C" void effect_shader_reindex_pixel_shader(TransparentGeometryGroup *group) noexcept {
        ShaderEffect *shader = reinterpret_cast<ShaderEffect *>(group->shader);
        effect_shader_permutation_index = SHADER_EFFECT_EFFECT_MULTITEXTURE_NONLINEAR_TINT;

        if(shader->effect.secondary_map.tag_id == TagID::null_id() || shader->effect.secondary_map_anchor == SHADER_EFFECT_PARTICLE_ANCHOR_ZSPRITE || d3d9_device_caps->PixelShaderVersion < 0xffff0200) {
            effect_shader_permutation_index += 12;
        }
        else {
            // Handle 2nd map
            rasterizer_set_texture(1, BITMAP_DATA_TYPE_2D, BITMAP_USAGE_MULTIPLICATIVE, group->shader_permutation_index, shader->effect.secondary_map.tag_id);
            rasterizer_set_sampler_state(1, D3DSAMP_ADDRESSU, TEST_FLAG(shader->effect.secondary_map_flags, SHADER_EFFECT_MAP_U_CLAMP_BIT) ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP);
            rasterizer_set_sampler_state(1, D3DSAMP_ADDRESSV, TEST_FLAG(shader->effect.secondary_map_flags, SHADER_EFFECT_MAP_V_CLAMP_BIT) ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP);
            rasterizer_set_sampler_state(1, D3DSAMP_MAGFILTER, TEST_FLAG(shader->effect.secondary_map_flags, SHADER_EFFECT_MAP_FLAGS_POINT_SAMPLED_BIT) ? D3DTEXF_POINT : D3DTEXF_LINEAR);
            rasterizer_set_sampler_state(1, D3DSAMP_MINFILTER, TEST_FLAG(shader->effect.secondary_map_flags, SHADER_EFFECT_MAP_FLAGS_POINT_SAMPLED_BIT) ? D3DTEXF_POINT : D3DTEXF_LINEAR);
            rasterizer_set_sampler_state(1, D3DSAMP_MIPFILTER, TEST_FLAG(shader->effect.secondary_map_flags, SHADER_EFFECT_MAP_FLAGS_POINT_SAMPLED_BIT) ? D3DTEXF_POINT : D3DTEXF_LINEAR);
        }

        if(!TEST_FLAG(shader->effect.flags, SHADER_EFFECT_FLAGS_USES_NONLINEAR_TINT_BIT)) {
            effect_shader_permutation_index += 6;
        }

        if(!TEST_FLAG(group->geometry_flags, RASTERIZER_GEOMETRY_FLAGS_NO_FOG_BIT)) {
            switch(shader->effect.framebuffer_blend_function) {
                case SHADER_FRAMEBUFFER_BLEND_FUNCTION_ALPHA_BLEND:
                    effect_shader_permutation_index += 2;
                    break;
                case SHADER_FRAMEBUFFER_BLEND_FUNCTION_MULTIPLY:
                case SHADER_FRAMEBUFFER_BLEND_FUNCTION_MIN:
                    effect_shader_permutation_index += 4;
                    break;
                case SHADER_FRAMEBUFFER_BLEND_FUNCTION_DOUBLE_MULTIPLY:
                    effect_shader_permutation_index += 3;
                    break;
                case SHADER_FRAMEBUFFER_BLEND_FUNCTION_ADD:
                case SHADER_FRAMEBUFFER_BLEND_FUNCTION_REVERSE_SUBTRACT:
                case SHADER_FRAMEBUFFER_BLEND_FUNCTION_MAX:
                    effect_shader_permutation_index += 1;
                    break;
                case SHADER_FRAMEBUFFER_BLEND_FUNCTION_ALPHA_MULTIPLY_ADD:
                    effect_shader_permutation_index += 5;
                    break;
                default:
                    break;
            }
        }

        // +1 on retail/demo
        if(game_engine() != GameEngine::GAME_ENGINE_CUSTOM_EDITION) {
            effect_shader_permutation_index++;
            zsprite_drawn = true;
        }
    }

    extern "C" void set_up_zsprites(TransparentGeometryGroup *group) noexcept {
        if(d3d9_device_caps->PixelShaderVersion < 0xffff0200) {
            return;
        }

        ShaderEffect *shader = reinterpret_cast<ShaderEffect *>(group->shader);

        if(shader->effect.secondary_map_anchor == SHADER_EFFECT_PARTICLE_ANCHOR_ZSPRITE && !shader->effect.secondary_map.tag_id.is_null() && !TEST_FLAG(group->geometry_flags, RASTERIZER_GEOMETRY_FLAGS_FIRST_PERSON_BIT)) {
            float zsprite_radius_scale = (shader->effect.zsprite_radius_scale != 0.0f) ? shader->effect.zsprite_radius_scale : 1.0f;
            float zn = global_window_parameters->camera.z_near;
            float zf = global_window_parameters->camera.z_far;

            float q = zf / (zf- zn);
            float r = shader->effect.secondary_map_radius * zsprite_radius_scale;
            float w = -dot_product_3d(reinterpret_cast<Point3D *>(&global_window_parameters->camera.forward), &global_window_parameters->camera.position);

            float vsh_constants_zspite[] = {
                q, -q * zn, r, zn + 0.01f,
                global_window_parameters->camera.forward.i, global_window_parameters->camera.forward.j, global_window_parameters->camera.forward.k, w
            };

            int shader_index = game_engine() == GameEngine::GAME_ENGINE_CUSTOM_EDITION ? effect_shader_permutation_index : effect_shader_permutation_index - 1;
            shader_index = CHIMERA_PIXEL_SHADER_EFF_NLIN_TINT_Z + (shader_index - SHADER_EFFECT_EFFECT_NONLINEAR_TINT);

            IDirect3DDevice9_SetVertexShaderConstantF(*global_d3d9_device, 18, vsh_constants_zspite, 2);
            IDirect3DDevice9_SetVertexShader(*global_d3d9_device, rasterizer_get_vertex_shader(VSH_EFFECT_ZSPRITE));
            IDirect3DDevice9_SetVertexDeclaration(*global_d3d9_device, rasterizer_get_vertex_declaration(VERTEX_DECLARATION_UNLIT_ZSPRITE));
            IDirect3DDevice9_SetStreamSource(*global_d3d9_device, 1, *aux_buffer, 0, 8);
            IDirect3DDevice9_SetPixelShader(*global_d3d9_device, chimera_pixel_shaders[shader_index]);

            rasterizer_set_texture(1, BITMAP_DATA_TYPE_2D, BITMAP_USAGE_ADDITIVE, group->shader_permutation_index, shader->effect.secondary_map.tag_id);
            rasterizer_set_sampler_state(1, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
            rasterizer_set_sampler_state(1, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
            rasterizer_set_sampler_state(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            rasterizer_set_sampler_state(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            rasterizer_set_sampler_state(1, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);

            // Draw here on retail to skip d3dx snot.
            if(game_engine() != GameEngine::GAME_ENGINE_CUSTOM_EDITION) {
                rasterizer_transparent_geometry_group_draw_vertices(group, false);
                zsprite_drawn = true;
            }
        }
    }

    void meme_up_the_aux_buffer() noexcept {
        float *vertices = nullptr;
        IDirect3DVertexBuffer9_Lock(*aux_buffer, 0, 0x10000, reinterpret_cast<void **>(&vertices), 0);

        float sequence[] = { 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f };
        int sequence_size = 8;
        int count = RASTERIZER_TRANSPARENT_GEOMETRY_TEXCOORD_STREAM_SIZE;

        while (count > 0) {
            memcpy(vertices, sequence, sizeof(sequence));
            vertices += sequence_size;
            count -= sequence_size;
        }

        IDirect3DVertexBuffer9_Unlock(*aux_buffer);
    }

    void set_up_effect_shader_fix() noexcept {
        static Hook technique_hook, zprite_hook, aux_buffer_hook;
        write_function_override(get_chimera().get_signature("effect_shader_set_technique_sig").data(), technique_hook, reinterpret_cast<const void *>(effect_shader_reindex_pixel_shader_asm), &effect_shader_get_ps_original);

        aux_buffer = reinterpret_cast<IDirect3DVertexBuffer9 **>(*reinterpret_cast<std::byte **>(get_chimera().get_signature("transparent_geometry_aux_buffer_unlock_sig").data() + 1));

        if(game_engine() == GameEngine::GAME_ENGINE_CUSTOM_EDITION) {
            write_jmp_call(get_chimera().get_signature("effect_shader_draw_vertices_sig").data() + 10, zprite_hook, reinterpret_cast<const void *>(effect_shader_set_up_zsprite_custom_asm), nullptr);
        }
        else {
            write_function_override(get_chimera().get_signature("effect_shader_set_effect_sig").data(), zprite_hook, reinterpret_cast<const void *>(shader_set_up_zprite_retail_asm), &shader_effect_d3dx_begin);
            shader_effect_draw_end = get_chimera().get_signature("effect_shader_end_sig").data() + 11;
        }

        // Gearbox stuffed up setting up this vertex buffer, so lets just set it's values properly here.
        write_jmp_call(get_chimera().get_signature("transparent_geometry_aux_buffer_unlock_sig").data() + 8, aux_buffer_hook, nullptr, reinterpret_cast<const void *>(meme_up_the_aux_buffer));
    }
}
