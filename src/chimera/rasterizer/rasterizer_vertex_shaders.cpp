// SPDX-License-Identifier: GPL-3.0-only

#include "rasterizer_vertex_shaders.hpp"
#include "../chimera.hpp"
#include "../halo_data/shader_effects.hpp"
#include "../halo_data/game_variables.hpp"
#include "../halo_data/shaders/shader_blob.hpp"
#include "../signature/signature.hpp"
#include "../signature/hook.hpp"


namespace Chimera {
    DynamicVertex screen_vertices[4] = {
        -1.0f, 1.0, 0.0f, 0xffffffff, -1.0f, 1.0f,
        1.0f, 1.0f, 0.0f, 0xffffffff, 1.0f, 1.0f,
        1.0f, -1.0f, 0.0f, 0xffffffff, 1.0f, -1.0f,
        -1.0f, -1.0f, 0.0f, 0xffffffff, -1.0f, -1.0f
    };

    enum {
        NUMBER_OF_GENERIC_VERTEX_SHADERS = 11,
    };

    static IDirect3DVertexShader9 *generic_vertex_shaders_3_0[NUMBER_OF_GENERIC_VERTEX_SHADERS] = {nullptr};
    static bool vsh_initialized = false;
    static bool vsh_creation_failed = false;

    // Realistically this function only ever returns generic vertex shaders. The built-in one does the rest.
    IDirect3DVertexShader9 *rasterizer_get_vertex_shader(std::uint16_t index) noexcept {
        if(index >= NUM_OF_VERTEX_SHADERS || !vertex_shaders) {
            return nullptr;
        }

        // These are generic vertex shaders. We need to pass the 3_0 ones if we're drawing generic with ps_3_0.
        if(index >= VSH_TRANSPARENT_GENERIC && index <= VSH_TRANSPARENT_GENERIC_VIEWER_CENTERED_M && d3d9_device_caps && d3d9_device_caps->PixelShaderVersion >= 0xffff0300 && d3d9_device_caps->VertexShaderVersion >= 0xfffe0300) {
            // Make sure they've been created
            rasterizer_create_vertex_shaders_3_0();
            if(vsh_initialized) {
                return generic_vertex_shaders_3_0[index - VSH_TRANSPARENT_GENERIC];
            }
        }

        return vertex_shaders[index].shader;
    }

    IDirect3DVertexShader9 *rasterizer_get_vertex_shader_for_permutation(uint16_t vertex_shader_permutation, short vertex_type) noexcept {
        if(!vertex_shader_permutations || vertex_shader_permutation >= 6 || vertex_type < 0 || vertex_type >= NUM_OF_VERTEX_DECLARATIONS) {
            return nullptr;
        }
        return rasterizer_get_vertex_shader(vertex_shader_permutations[vertex_shader_permutation + static_cast<std::size_t>(vertex_type) * 6]);
    }

    IDirect3DVertexDeclaration9 *rasterizer_get_vertex_declaration(short vertex_type) noexcept {
        if(!vertex_declarations || vertex_type < 0 || vertex_type >= NUM_OF_VERTEX_DECLARATIONS) {
            return nullptr;
        }
        return vertex_declarations[vertex_type].declaration;
    }

    void rasterizer_create_vertex_shaders_3_0() noexcept {
        if(!vsh_initialized && !vsh_creation_failed && d3d9_device_caps && global_d3d9_device && *global_d3d9_device) {
            if(!(d3d9_device_caps->VertexShaderVersion < 0xfffe0300)) {
                auto create_shader = [](IDirect3DDevice9 *device, const void *shader_bytecode, IDirect3DVertexShader9 **shader) noexcept {
                    const auto result = IDirect3DDevice9_CreateVertexShader(device, reinterpret_cast<const DWORD *>(shader_bytecode), shader);
                    return SUCCEEDED(result) && *shader;
                };

                const bool created_all =
                    create_shader(*global_d3d9_device, vsh_transparent_generic, &generic_vertex_shaders_3_0[0]) &&
                    create_shader(*global_d3d9_device, vsh_transparent_generic_lit_m, &generic_vertex_shaders_3_0[1]) &&
                    create_shader(*global_d3d9_device, vsh_transparent_generic_m, &generic_vertex_shaders_3_0[2]) &&
                    create_shader(*global_d3d9_device, vsh_transparent_generic_object_centered, &generic_vertex_shaders_3_0[3]) &&
                    create_shader(*global_d3d9_device, vsh_transparent_generic_object_centered_m, &generic_vertex_shaders_3_0[4]) &&
                    create_shader(*global_d3d9_device, vsh_transparent_generic_reflection, &generic_vertex_shaders_3_0[5]) &&
                    create_shader(*global_d3d9_device, vsh_transparent_generic_reflection_m, &generic_vertex_shaders_3_0[6]) &&
                    create_shader(*global_d3d9_device, vsh_transparent_generic_screenspace, &generic_vertex_shaders_3_0[7]) &&
                    create_shader(*global_d3d9_device, vsh_transparent_generic_screenspace_m, &generic_vertex_shaders_3_0[8]) &&
                    create_shader(*global_d3d9_device, vsh_transparent_generic_viewer_centered, &generic_vertex_shaders_3_0[9]) &&
                    create_shader(*global_d3d9_device, vsh_transparent_generic_viewer_centered_m, &generic_vertex_shaders_3_0[10]);

                if(!created_all) {
                    for(auto &shader : generic_vertex_shaders_3_0) {
                        if(shader) {
                            IDirect3DVertexShader9_Release(shader);
                            shader = nullptr;
                        }
                    }
                    vsh_creation_failed = true;
                    return;
                }

                vsh_initialized = true;
            }
        }
    }

    void rasterizer_release_vertex_shaders_3_0() noexcept {
        for(auto &shader : generic_vertex_shaders_3_0) {
            if(shader) {
                IDirect3DVertexShader9_Release(shader);
                shader = nullptr;
            }
        }
        vsh_initialized = false;
        vsh_creation_failed = false;
    }

}
