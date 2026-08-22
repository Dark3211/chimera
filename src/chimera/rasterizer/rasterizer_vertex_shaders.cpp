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
        1.0f, 1.0, 0.0f, 0xffffffff, 1.0f, 1.0f,
        1.0f, -1.0f, 0.0f, 0xffffffff, 1.0f, -1.0f,
        -1.0f, -1.0f, 0.0f, 0xffffffff, -1.0f, -1.0f
    };

    // Full-size modern bank. A null slot means "use Halo's stock shader".
    // The existing transparent-generic VS3 shaders populate the first modern
    // family. Additional converted shaders can now be added by index without
    // changing the selection API or disturbing already working stock paths.
    static IDirect3DVertexShader9 *modern_vertex_shaders_3_0[NUM_OF_VERTEX_SHADERS] = {nullptr};
    static bool vsh_initialized = false;
    static bool vsh_creation_failed = false;

    static bool modern_shader_caps_available() noexcept {
        return d3d9_device_caps
            && d3d9_device_caps->PixelShaderVersion >= 0xffff0300
            && d3d9_device_caps->VertexShaderVersion >= 0xfffe0300;
    }

    IDirect3DVertexShader9 *rasterizer_get_modern_vertex_shader(std::uint16_t index) noexcept {
        if(index >= NUM_OF_VERTEX_SHADERS || !modern_shader_caps_available()) {
            return nullptr;
        }
        rasterizer_create_vertex_shaders_3_0();
        if(!vsh_initialized) {
            return nullptr;
        }
        return modern_vertex_shaders_3_0[index];
    }

    bool rasterizer_has_modern_vertex_shader(std::uint16_t index) noexcept {
        return rasterizer_get_modern_vertex_shader(index) != nullptr;
    }

    std::size_t rasterizer_modern_vertex_shader_count() noexcept {
        if(!modern_shader_caps_available()) {
            return 0;
        }
        rasterizer_create_vertex_shaders_3_0();
        if(!vsh_initialized) {
            return 0;
        }
        std::size_t count = 0;
        for(auto *shader : modern_vertex_shaders_3_0) {
            if(shader) {
                count++;
            }
        }
        return count;
    }

    IDirect3DVertexShader9 *rasterizer_get_vertex_shader(std::uint16_t index) noexcept {
        if(index >= NUM_OF_VERTEX_SHADERS || !vertex_shaders) {
            return nullptr;
        }

        if(IDirect3DVertexShader9 *modern = rasterizer_get_modern_vertex_shader(index)) {
            return modern;
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
        if(vsh_initialized || vsh_creation_failed || !d3d9_device_caps || !global_d3d9_device || !*global_d3d9_device) {
            return;
        }
        if(d3d9_device_caps->VertexShaderVersion < 0xfffe0300) {
            return;
        }

        auto create_shader = [](IDirect3DDevice9 *device, const void *shader_bytecode, IDirect3DVertexShader9 **shader) noexcept {
            if(!device || !shader_bytecode || !shader) {
                return false;
            }
            const auto result = IDirect3DDevice9_CreateVertexShader(
                device,
                reinterpret_cast<const DWORD *>(shader_bytecode),
                shader
            );
            return SUCCEEDED(result) && *shader;
        };

        IDirect3DDevice9 *device = *global_d3d9_device;
        const bool created_all =
            create_shader(device, vsh_transparent_generic,
                &modern_vertex_shaders_3_0[VSH_TRANSPARENT_GENERIC]) &&
            create_shader(device, vsh_transparent_generic_lit_m,
                &modern_vertex_shaders_3_0[VSH_TRANSPARENT_GENERIC_LIT_M]) &&
            create_shader(device, vsh_transparent_generic_m,
                &modern_vertex_shaders_3_0[VSH_TRANSPARENT_GENERIC_M]) &&
            create_shader(device, vsh_transparent_generic_object_centered,
                &modern_vertex_shaders_3_0[VSH_TRANSPARENT_GENERIC_OBJECT_CENTERED]) &&
            create_shader(device, vsh_transparent_generic_object_centered_m,
                &modern_vertex_shaders_3_0[VSH_TRANSPARENT_GENERIC_OBJECT_CENTERED_M]) &&
            create_shader(device, vsh_transparent_generic_reflection,
                &modern_vertex_shaders_3_0[VSH_TRANSPARENT_GENERIC_REFLECTION]) &&
            create_shader(device, vsh_transparent_generic_reflection_m,
                &modern_vertex_shaders_3_0[VSH_TRANSPARENT_GENERIC_REFLECTION_M]) &&
            create_shader(device, vsh_transparent_generic_screenspace,
                &modern_vertex_shaders_3_0[VSH_TRANSPARENT_GENERIC_SCREENSPACE]) &&
            create_shader(device, vsh_transparent_generic_screenspace_m,
                &modern_vertex_shaders_3_0[VSH_TRANSPARENT_GENERIC_SCREENSPACE_M]) &&
            create_shader(device, vsh_transparent_generic_viewer_centered,
                &modern_vertex_shaders_3_0[VSH_TRANSPARENT_GENERIC_VIEWER_CENTERED]) &&
            create_shader(device, vsh_transparent_generic_viewer_centered_m,
                &modern_vertex_shaders_3_0[VSH_TRANSPARENT_GENERIC_VIEWER_CENTERED_M]);

        if(!created_all) {
            for(auto &shader : modern_vertex_shaders_3_0) {
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

    void rasterizer_release_vertex_shaders_3_0() noexcept {
        for(auto &shader : modern_vertex_shaders_3_0) {
            if(shader) {
                IDirect3DVertexShader9_Release(shader);
                shader = nullptr;
            }
        }
        vsh_initialized = false;
        vsh_creation_failed = false;
    }

}
