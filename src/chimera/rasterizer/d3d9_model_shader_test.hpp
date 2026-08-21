// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_D3D9_MODEL_SHADER_TEST_HPP
#define CHIMERA_D3D9_MODEL_SHADER_TEST_HPP

#include <windows.h>
#include <d3d9.h>

#include <cstddef>

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

        static SetVertexShaderFunction original_set_vertex_shader = nullptr;
        static IDirect3DDevice9 *installed_device = nullptr;
        static bool announced = false;

        static bool enabled() noexcept {
            auto *backend = get_chimera().get_ini()->get_value("video_mode.d3d_backend");
            if(!backend || (_stricmp(backend, "9on12") != 0 && _stricmp(backend, "d3d9on12") != 0)) {
                return false;
            }

            auto *mode = get_chimera().get_ini()->get_value("video_mode.d3d_model_shader_test");
            return mode && _stricmp(mode, "fast_to_model") == 0;
        }

        static HRESULT STDMETHODCALLTYPE set_vertex_shader_hook(
            IDirect3DDevice9 *device,
            IDirect3DVertexShader9 *shader
        ) {
            if(!original_set_vertex_shader) {
                return D3DERR_INVALIDCALL;
            }

            if(enabled() && vertex_shaders
                && vertex_shaders[VSH_MODEL_FAST].shader
                && vertex_shaders[VSH_MODEL].shader
                && shader == vertex_shaders[VSH_MODEL_FAST].shader) {
                shader = vertex_shaders[VSH_MODEL].shader;
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
                console_output("D3D9 backend: testing VSH_MODEL_FAST -> VSH_MODEL substitution on D3D9On12.");
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
