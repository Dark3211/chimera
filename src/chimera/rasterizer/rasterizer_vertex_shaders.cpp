// SPDX-License-Identifier: GPL-3.0-only

#include <cstddef>

#include "rasterizer_vertex_shaders.hpp"
#include "../fix/internal_shaders.hpp"
#include "../halo_data/shader_effects.hpp"
#include "../halo_data/game_variables.hpp"
#include "../halo_data/shaders/shader_blob.hpp"


namespace Chimera {
    DynamicVertex screen_vertices[4] = {
        -1.0f, 1.0, 0.0f, 0xffffffff, -1.0f, 1.0f,
        1.0f, 1.0, 0.0f, 0xffffffff, 1.0f, 1.0f,
        1.0f, -1.0f, 0.0f, 0xffffffff, 1.0f, -1.0f,
        -1.0f, -1.0f, 0.0f, 0xffffffff, -1.0f, -1.0f
    };

    enum {
        NUMBER_OF_GENERIC_VERTEX_SHADERS = 11,
    };

    static IDirect3DVertexShader9 *generic_vertex_shaders_3_0[NUMBER_OF_GENERIC_VERTEX_SHADERS] = {nullptr};
    static bool vsh_initialized = false;

    IDirect3DVertexShader9 *rasterizer_get_vertex_shader(std::uint16_t index) noexcept {
        if(index >= NUM_OF_VERTEX_SHADERS || !vertex_shaders) {
            return nullptr;
        }

        // GENERIC_M is deliberately VS2 in the validated D3D9On12 collection.
        // Do not replace that slot with Chimera's generic VS3 helper on 9On12.
        const bool keep_d3d9on12_generic_m =
            using_internal_d3d9on12_vertex_shader_collection()
            && index == VSH_TRANSPARENT_GENERIC_M;

        if(!keep_d3d9on12_generic_m
            && index >= VSH_TRANSPARENT_GENERIC
            && index <= VSH_TRANSPARENT_GENERIC_VIEWER_CENTERED_M
            && d3d9_device_caps
            && d3d9_device_caps->PixelShaderVersion >= 0xffff0300
            && d3d9_device_caps->VertexShaderVersion >= 0xfffe0300) {
            rasterizer_create_vertex_shaders_3_0();
            if(vsh_initialized) {
                return generic_vertex_shaders_3_0[index - VSH_TRANSPARENT_GENERIC];
            }
        }

        return vertex_shaders[index].shader;
    }

    IDirect3DVertexShader9 *rasterizer_get_vertex_shader_for_permutation(std::uint16_t vertex_shader_permutation, short vertex_type) noexcept {
        if(!vertex_shader_permutations
            || vertex_shader_permutation >= 6
            || vertex_type < 0
            || vertex_type >= NUM_OF_VERTEX_DECLARATIONS) {
            return nullptr;
        }

        return rasterizer_get_vertex_shader(
            vertex_shader_permutations[vertex_shader_permutation + static_cast<std::size_t>(vertex_type) * 6]
        );
    }

    IDirect3DVertexDeclaration9 *rasterizer_get_vertex_declaration(short vertex_type) noexcept {
        if(!vertex_declarations || vertex_type < 0 || vertex_type >= NUM_OF_VERTEX_DECLARATIONS) {
            return nullptr;
        }
        return vertex_declarations[vertex_type].declaration;
    }

    void rasterizer_create_vertex_shaders_3_0() noexcept {
        if(vsh_initialized
            || !d3d9_device_caps
            || !global_d3d9_device
            || !*global_d3d9_device
            || d3d9_device_caps->VertexShaderVersion < 0xfffe0300) {
            return;
        }

        const void *bytecode[NUMBER_OF_GENERIC_VERTEX_SHADERS] = {
            vsh_transparent_generic,
            vsh_transparent_generic_lit_m,
            vsh_transparent_generic_m,
            vsh_transparent_generic_object_centered,
            vsh_transparent_generic_object_centered_m,
            vsh_transparent_generic_reflection,
            vsh_transparent_generic_reflection_m,
            vsh_transparent_generic_screenspace,
            vsh_transparent_generic_screenspace_m,
            vsh_transparent_generic_viewer_centered,
            vsh_transparent_generic_viewer_centered_m
        };

        const bool d3d9on12_collection = using_internal_d3d9on12_vertex_shader_collection();
        const std::size_t generic_m_slot =
            static_cast<std::size_t>(VSH_TRANSPARENT_GENERIC_M - VSH_TRANSPARENT_GENERIC);

        IDirect3DDevice9 *device = *global_d3d9_device;
        for(std::size_t i = 0; i < NUMBER_OF_GENERIC_VERTEX_SHADERS; i++) {
            // The internal 9On12 collection already supplies the validated exact
            // VS2 GENERIC_M, so do not create an unused VS3 replacement for it.
            if(d3d9on12_collection && i == generic_m_slot) {
                continue;
            }

            const HRESULT result = IDirect3DDevice9_CreateVertexShader(
                device,
                reinterpret_cast<const DWORD *>(bytecode[i]),
                &generic_vertex_shaders_3_0[i]
            );
            if(FAILED(result) || !generic_vertex_shaders_3_0[i]) {
                rasterizer_release_vertex_shaders_3_0();
                return;
            }
        }

        vsh_initialized = true;
    }

    void rasterizer_release_vertex_shaders_3_0() noexcept {
        for(auto &shader : generic_vertex_shaders_3_0) {
            if(shader) {
                IDirect3DVertexShader9_Release(shader);
                shader = nullptr;
            }
        }
        vsh_initialized = false;
    }

}
