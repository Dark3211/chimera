// SPDX-License-Identifier: GPL-3.0-only

#include "decal_fix.hpp"
#include "../chimera.hpp"
#include "../signature/signature.hpp"
#include "../signature/hook.hpp"
#include "../halo_data/shader_defs.hpp"
#include "../halo_data/game_variables.hpp"
#include "../halo_data/decal.hpp"
#include "../halo_data/map.hpp"
#include "../rasterizer/rasterizer.hpp"
#include "../event/map_load.hpp"
#include "../event/tick.hpp"
#include "../event/game_loop.hpp"

namespace Chimera {
    extern "C" {
        void fix_decal_shaders_asm() noexcept;
    }

    #define DECAL_VERTEX_BUFFER_SIZE (2048 * 5 * sizeof(DecalVertex) * 1.5)

    struct DecalVertexBufferCache {
        char map_name[32];
        std::int32_t tick_count;
        void *cache;
    };

    static DecalVertexBufferCache decal_vertex_cache = { "", -1, nullptr };
    IDirect3DVertexBuffer9 **decal_vertex_buffer = nullptr;
    static bool map_loaded = false;
    static bool can_update_cache = false;
    static bool *fog_hack = nullptr;

    extern "C" void set_up_pixel_shader_for_decals(ShaderDecal *shader) noexcept {
        if(!shader || !d3d9_device_caps || !global_d3d9_device || !*global_d3d9_device) {
            return;
        }

        // If we can, use a ps2.0 pixel shader instead of the fixed function texture blending.
        if(d3d9_device_caps->PixelShaderVersion < 0xffff0200) {
            return;
        }

        IDirect3DPixelShader9 *pixel_shader = nullptr;
        float ps_constants[4] = {0};

        // This could be done in a simgle shader, but is inefficient under ps2.0 due to lack of proper branching.
        switch(shader->framebuffer_blend_function) {
            case SHADER_FRAMEBUFFER_BLEND_FUNCTION_ADD:
            case SHADER_FRAMEBUFFER_BLEND_FUNCTION_REVERSE_SUBTRACT:
            case SHADER_FRAMEBUFFER_BLEND_FUNCTION_MAX:
                pixel_shader = chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_DECAL_ADD];
                break;
            case SHADER_FRAMEBUFFER_BLEND_FUNCTION_MULTIPLY:
            case SHADER_FRAMEBUFFER_BLEND_FUNCTION_MIN:
                pixel_shader = chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_DECAL_MULTIPLY];
                break;
            case SHADER_FRAMEBUFFER_BLEND_FUNCTION_DOUBLE_MULTIPLY:
                pixel_shader = chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_DECAL_MULTIPLY2X];
                break;
            case SHADER_FRAMEBUFFER_BLEND_FUNCTION_ALPHA_BLEND:
                pixel_shader = chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_DECAL_ALPHA_BLEND];
                break;
            case SHADER_FRAMEBUFFER_BLEND_FUNCTION_ALPHA_MULTIPLY_ADD:
                pixel_shader = chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_DECAL_ALPHA_MULTIPLY_ADD];
                break;
            default:
                break;
        }
        
        ps_constants[3] = fog_hack && *fog_hack ? (1.0f / 255.0f) : 0.0f;

        IDirect3DDevice9_SetPixelShader(*global_d3d9_device, pixel_shader);
        IDirect3DDevice9_SetPixelShaderConstantF(*global_d3d9_device, 0, ps_constants, 1);
        IDirect3DDevice9_SetTextureStageState(*global_d3d9_device, 0, D3DTSS_COLOROP, D3DTOP_DISABLE);
        IDirect3DDevice9_SetTextureStageState(*global_d3d9_device, 0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    }

    void flush_vertex_cache(DecalVertexBufferCache *vertex_cache) noexcept {
        static bool initialized = false;
        if(!vertex_cache || !vertex_cache->cache) {
            return;
        }

        if(!initialized) {
            memset(vertex_cache->cache, 0, DECAL_VERTEX_BUFFER_SIZE);
            memset(&vertex_cache->map_name, 0, 32);
            vertex_cache->tick_count = -1;
            initialized = true;
        }
        else {
            // No need to flush if it's already in a flushed state.
            if(vertex_cache->tick_count != -1) {
                memset(vertex_cache->cache, 0, DECAL_VERTEX_BUFFER_SIZE);
                memset(&vertex_cache->map_name, 0, 32);
                vertex_cache->tick_count = -1;
            }
        }
    }

    void back_up_decal_vertices() noexcept {
        if(!can_update_cache || !decal_vertex_cache.cache || !decal_vertex_buffer || !*decal_vertex_buffer) {
            return;
        }

        void *vertices = nullptr;
        const HRESULT lock_result = IDirect3DVertexBuffer9_Lock(
            *decal_vertex_buffer,
            0,
            DECAL_VERTEX_BUFFER_SIZE,
            &vertices,
            D3DLOCK_READONLY
        );
        if(SUCCEEDED(lock_result)) {
            if(vertices) {
                memcpy(decal_vertex_cache.cache, vertices, DECAL_VERTEX_BUFFER_SIZE);
                decal_vertex_cache.tick_count = get_tick_count();
                memcpy(decal_vertex_cache.map_name, get_map_name(), 32);
            }
            IDirect3DVertexBuffer9_Unlock(*decal_vertex_buffer);
        }
    }

    void restore_decal_vertices() noexcept {
        if(!game_state_globals || !decal_vertex_cache.cache || !decal_vertex_buffer || !*decal_vertex_buffer) {
            return;
        }

        std::int32_t tick_count = get_tick_count();
        if(game_state_globals->revert_time == tick_count && tick_count > 0) {
            if(strncmp(decal_vertex_cache.map_name, get_map_name(), 32) != 0 || decal_vertex_cache.tick_count != tick_count) {
                // The cache doesn't match reality, so wipe it.
                flush_vertex_cache(&decal_vertex_cache);
            }
            void *vertices = nullptr;
            const HRESULT lock_result = IDirect3DVertexBuffer9_Lock(
                *decal_vertex_buffer,
                0,
                DECAL_VERTEX_BUFFER_SIZE,
                &vertices,
                0
            );
            if(SUCCEEDED(lock_result)) {
                if(vertices) {
                    memcpy(vertices, decal_vertex_cache.cache, DECAL_VERTEX_BUFFER_SIZE);
                }
                IDirect3DVertexBuffer9_Unlock(*decal_vertex_buffer);
            }
        }
    }

    void set_can_update_cache_flag() noexcept {
        if(map_loaded) {
            can_update_cache = true;
        }
    }

    void set_map_loaded_flag() noexcept {
        // We don't want to be able to update the cache straight away after map load in case we're loading a checkpoint.
        can_update_cache = false;
        map_loaded = true;
    }

    void dispose_vertex_cache() noexcept {
        if(decal_vertex_cache.cache) {
            GlobalFree(decal_vertex_cache.cache);
            decal_vertex_cache.cache = nullptr;
        }
        can_update_cache = false;
        map_loaded = false;
    }

    void set_up_decals_fix() noexcept {
        // Fix the pixel shader (or lack thereof).
        static Hook hook;
        write_jmp_call(get_chimera().get_signature("decal_draw_vertices_sig").data(), hook, reinterpret_cast<const void *>(fix_decal_shaders_asm), nullptr);
        fog_hack = *reinterpret_cast<bool **>(get_chimera().get_signature("decal_fog_hack_sig").data() + 2);

        // Basically copy the vertex buffer contents to a cache upon creating a checkpoint
        // and writing it back on game revert. Xbox allocates the vertex buffer into the game
        // state memory pool instead to prevent dirty vertices being used.
        decal_vertex_cache.cache = GlobalAlloc(GMEM_FIXED, DECAL_VERTEX_BUFFER_SIZE);
        if(decal_vertex_cache.cache) {
            flush_vertex_cache(&decal_vertex_cache);
            decal_vertex_buffer = *reinterpret_cast<IDirect3DVertexBuffer9 ***>(get_chimera().get_signature("decal_vertex_buffer_sig").data() + 1);

            // The things I do to avoid writing to game state...
            std::uint32_t *game_state_before_save_ptr = *reinterpret_cast<std::uint32_t **>(get_chimera().get_signature("game_state_before_save_sig").data() + 2);
            overwrite(game_state_before_save_ptr, &back_up_decal_vertices);
            add_pretick_event(restore_decal_vertices);
            add_tick_event(set_can_update_cache_flag);
            add_map_load_event(set_map_loaded_flag);
            add_game_exit_event(dispose_vertex_cache);
        }
    }
}
