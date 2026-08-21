// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_D3D9_MODEL_SHADER_TEST_HPP
#define CHIMERA_D3D9_MODEL_SHADER_TEST_HPP

#include <windows.h>
#include <d3d9.h>

#include <cstddef>
#include <cstdint>

#include "../chimera.hpp"
#include "../config/ini.hpp"
#include "../halo_data/game_variables.hpp"
#include "../halo_data/shader_effects.hpp"
#include "../output/output.hpp"

namespace Chimera {
    namespace D3D9ModelShaderTest {
        constexpr std::size_t DEVICE_SET_VERTEX_SHADER = 92;

        using SetVertexShaderFunction = HRESULT (STDMETHODCALLTYPE *)(
            IDirect3DDevice9 *, IDirect3DVertexShader9 *
        );

        enum class Mode {
            OFF,
            FAST_TO_MODEL,
            FAST_TO_SCENERY,
            MODEL_FAMILY_TO_SCENERY
        };

        static SetVertexShaderFunction original_set_vertex_shader = nullptr;
        static IDirect3DDevice9 *installed_device = nullptr;
        static bool announced = false;
        static std::uint32_t family_hit_mask = 0;

        static bool d3d9on12_requested() noexcept {
            auto *backend = get_chimera().get_ini()->get_value("video_mode.d3d_backend");
            return backend
                && (_stricmp(backend, "9on12") == 0 || _stricmp(backend, "d3d9on12") == 0);
        }

        static Mode mode() noexcept {
            if(!d3d9on12_requested()) {
                return Mode::OFF;
            }

            auto *value = get_chimera().get_ini()->get_value("video_mode.d3d_model_shader_test");
            if(!value) {
                return Mode::OFF;
            }
            if(_stricmp(value, "fast_to_model") == 0) {
                return Mode::FAST_TO_MODEL;
            }
            if(_stricmp(value, "fast_to_scenery") == 0) {
                return Mode::FAST_TO_SCENERY;
            }
            if(_stricmp(value, "model_family_to_scenery") == 0) {
                return Mode::MODEL_FAMILY_TO_SCENERY;
            }
            return Mode::OFF;
        }

        static bool enabled() noexcept {
            return mode() != Mode::OFF;
        }

        static void announce_family_hit(std::uint32_t bit, const char *name) noexcept {
            if(family_hit_mask & bit) {
                return;
            }
            family_hit_mask |= bit;
            console_output("D3D9 model shader isolation: replacing %s with VSH_MODEL_SCENERY.", name);
        }

        static bool substitute_model_family_shader(IDirect3DVertexShader9 *&shader) noexcept {
            if(!vertex_shaders || !vertex_shaders[VSH_MODEL_SCENERY].shader) {
                return false;
            }

            struct Candidate {
                VertexShaderIndex index;
                std::uint32_t bit;
                const char *name;
            };

            // These are the ordinary visible-model shaders that share the legacy
            // multi-bone model path. MODEL_SCENERY is deliberately excluded because
            // it is our single-bone control shader.
            static constexpr Candidate candidates[] = {
                {VSH_MODEL_FOGGED, 1u << 0, "VSH_MODEL_FOGGED"},
                {VSH_MODEL,        1u << 1, "VSH_MODEL"},
                {VSH_MODEL_FF,     1u << 2, "VSH_MODEL_FF"},
                {VSH_MODEL_FAST,   1u << 3, "VSH_MODEL_FAST"},
            };

            for(const auto &candidate : candidates) {
                if(vertex_shaders[candidate.index].shader
                    && shader == vertex_shaders[candidate.index].shader) {
                    announce_family_hit(candidate.bit, candidate.name);
                    shader = vertex_shaders[VSH_MODEL_SCENERY].shader;
                    return true;
                }
            }
            return false;
        }

        static HRESULT STDMETHODCALLTYPE set_vertex_shader_hook(
            IDirect3DDevice9 *device,
            IDirect3DVertexShader9 *shader
        ) {
            if(!original_set_vertex_shader) {
                return D3DERR_INVALIDCALL;
            }

            switch(mode()) {
                case Mode::FAST_TO_MODEL:
                    if(vertex_shaders && vertex_shaders[VSH_MODEL_FAST].shader
                        && vertex_shaders[VSH_MODEL].shader
                        && shader == vertex_shaders[VSH_MODEL_FAST].shader) {
                        shader = vertex_shaders[VSH_MODEL].shader;
                    }
                    break;

                case Mode::FAST_TO_SCENERY:
                    if(vertex_shaders && vertex_shaders[VSH_MODEL_FAST].shader
                        && vertex_shaders[VSH_MODEL_SCENERY].shader
                        && shader == vertex_shaders[VSH_MODEL_FAST].shader) {
                        // model_scenery compiles ModelVS(true, false): it uses
                        // c_node_matrices[0] directly instead of BlendIndices-driven
                        // relative constant addressing. Visual rigidity is expected.
                        shader = vertex_shaders[VSH_MODEL_SCENERY].shader;
                    }
                    break;

                case Mode::MODEL_FAMILY_TO_SCENERY:
                    // Broader isolation after FAST_TO_SCENERY proved insufficient.
                    // Replace all ordinary visible-model multi-bone variants so a
                    // spike cannot simply come from MODEL/MODEL_FOGGED/MODEL_FF
                    // while MODEL_FAST alone is under test.
                    substitute_model_family_shader(shader);
                    break;

                case Mode::OFF:
                    break;
            }

            return original_set_vertex_shader(device, shader);
        }

        static bool install(IDirect3DDevice9 *device) noexcept {
            if(!device || !enabled()) {
                return false;
            }

            auto *vtable = *reinterpret_cast<ULONG_PTR **>(device);
            if(!vtable) {
                return false;
            }

            auto *entry = &vtable[DEVICE_SET_VERTEX_SHADER];
            const ULONG_PTR replacement = reinterpret_cast<ULONG_PTR>(set_vertex_shader_hook);

            if(*entry == replacement && installed_device == device && original_set_vertex_shader) {
                return true;
            }

            // Do not chain this diagnostic experiment on top of another unknown
            // SetVertexShader hook. Run it with d3d_diag=off so the experiment has
            // one controlled variable.
            original_set_vertex_shader = reinterpret_cast<SetVertexShaderFunction>(*entry);
            if(!original_set_vertex_shader) {
                return false;
            }

            DWORD old_protection = 0;
            if(!VirtualProtect(entry, sizeof(*entry), PAGE_EXECUTE_READWRITE, &old_protection)) {
                original_set_vertex_shader = nullptr;
                return false;
            }

            *entry = replacement;
            DWORD ignored = 0;
            VirtualProtect(entry, sizeof(*entry), old_protection, &ignored);
            FlushInstructionCache(GetCurrentProcess(), entry, sizeof(*entry));
            installed_device = device;

            if(!announced) {
                switch(mode()) {
                    case Mode::FAST_TO_SCENERY:
                        console_output("D3D9 backend: testing VSH_MODEL_FAST -> VSH_MODEL_SCENERY single-bone isolation on D3D9On12.");
                        break;
                    case Mode::MODEL_FAMILY_TO_SCENERY:
                        console_output("D3D9 backend: testing visible model shader family -> VSH_MODEL_SCENERY single-bone isolation on D3D9On12.");
                        break;
                    case Mode::FAST_TO_MODEL:
                        console_output("D3D9 backend: testing VSH_MODEL_FAST -> VSH_MODEL substitution on D3D9On12.");
                        break;
                    case Mode::OFF:
                        break;
                }
                announced = true;
            }
            return true;
        }

        static void set_up() noexcept {
            if(!enabled() || !global_d3d9_device || !*global_d3d9_device) {
                return;
            }
            install(*global_d3d9_device);
        }
    }

    inline void set_up_d3d9_model_shader_test() noexcept {
        D3D9ModelShaderTest::set_up();
    }
}

#endif
