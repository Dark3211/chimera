// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_D3D9_MODEL_MATERIAL_DIAG_HPP
#define CHIMERA_D3D9_MODEL_MATERIAL_DIAG_HPP

#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "d3d9_model_shader_primary_v2.hpp"
#include "../chimera.hpp"
#include "../config/ini.hpp"
#include "../event/d3d9_end_scene.hpp"
#include "../halo_data/game_variables.hpp"
#include "../halo_data/shader_effects.hpp"
#include "../output/output.hpp"

namespace Chimera {
    namespace D3D9ModelMaterialDiag {
        constexpr std::size_t DEVICE_SET_PIXEL_SHADER = 107;

        using SetPixelShaderFunction = HRESULT (STDMETHODCALLTYPE *)(
            IDirect3DDevice9 *, IDirect3DPixelShader9 *
        );

        static SetPixelShaderFunction original_set_pixel_shader = nullptr;
        static IDirect3DDevice9 *installed_device = nullptr;
        static IDirect3DPixelShader9 *base_s0_shader = nullptr;
        static bool queued_announced = false;
        static bool installed_announced = false;
        static bool replacement_announced = false;
        static bool end_scene_retry_registered = false;
        static std::FILE *pass_log = nullptr;
        static std::uint32_t log_flush_counter = 0;

        static constexpr const char *base_s0_hlsl = R"HLSL(
sampler2D BaseSampler : register(s0);

struct PS_INPUT {
    float4 T1 : TEXCOORD1;
};

float4 main(PS_INPUT IN) : COLOR0 {
    return tex2D(BaseSampler, IN.T1.xy);
}
)HLSL";

        static bool d3d9on12_requested() noexcept {
            auto *backend = get_chimera().get_ini()->get_value("video_mode.d3d_backend");
            return backend
                && (_stricmp(backend, "9on12") == 0 || _stricmp(backend, "d3d9on12") == 0);
        }

        static bool enabled() noexcept {
            if(!d3d9on12_requested() || !D3D9ModelShaderPrimaryV2::enabled()) {
                return false;
            }

            auto *value = get_chimera().get_ini()->get_value("video_mode.d3d_model_material_test");
            return value && (
                _stricmp(value, "base_s0") == 0
                || _stricmp(value, "texture0") == 0
                || _stricmp(value, "base_texture") == 0
            );
        }

        static void ensure_pass_log() noexcept {
            if(pass_log) {
                return;
            }
            pass_log = std::fopen("chimera_d3d9_model_pass.log", "w");
            if(pass_log) {
                std::fprintf(pass_log, "# frame\tvertex_shader_pass\tmaterial_ps\n");
                std::fflush(pass_log);
            }
            else {
                console_output("D3D9 material diagnostic: could not create chimera_d3d9_model_pass.log.");
            }
        }

        static const char *classify_vertex_shader(IDirect3DVertexShader9 *shader, bool &primary) noexcept {
            primary = false;
            if(!shader) {
                return nullptr;
            }

            if(shader == D3D9ModelShaderPrimaryV2::model_shader) {
                primary = true;
                return "PRIMARY_VS2_MODEL";
            }
            if(shader == D3D9ModelShaderPrimaryV2::fogged_shader) {
                primary = true;
                return "PRIMARY_VS2_FOGGED";
            }

            if(!vertex_shaders) {
                return nullptr;
            }

            struct Candidate {
                VertexShaderIndex index;
                const char *name;
            };
            static constexpr Candidate candidates[] = {
                {VSH_MODEL_FOGGED,               "VSH_MODEL_FOGGED_STOCK"},
                {VSH_MODEL,                      "VSH_MODEL_STOCK"},
                {VSH_MODEL_FF,                   "VSH_MODEL_FF"},
                {VSH_MODEL_FAST,                 "VSH_MODEL_FAST_STOCK"},
                {VSH_MODEL_SCENERY,              "VSH_MODEL_SCENERY"},
                {VSH_MODEL_ACTIVE_CAMOUFLAGE,    "VSH_MODEL_ACTIVE_CAMOUFLAGE"},
                {VSH_MODEL_ACTIVE_CAMOUFLAGE_FF, "VSH_MODEL_ACTIVE_CAMOUFLAGE_FF"},
                {VSH_MODEL_FOG_SCREEN,           "VSH_MODEL_FOG_SCREEN"},
                {VSH_MODEL_SHADOW,               "VSH_MODEL_SHADOW"},
                {VSH_MODEL_ZBUFFER,              "VSH_MODEL_ZBUFFER"},
            };

            for(const auto &candidate : candidates) {
                if(vertex_shaders[candidate.index].shader == shader) {
                    return candidate.name;
                }
            }
            return nullptr;
        }

        static void log_pass(const char *pass_name, bool material_replaced) noexcept {
            if(!pass_name) {
                return;
            }
            ensure_pass_log();
            if(!pass_log) {
                return;
            }

            const long long frame = rasterizer_globals
                ? static_cast<long long>(rasterizer_globals->frame_index)
                : -1LL;
            std::fprintf(
                pass_log,
                "%lld\t%s\t%s\n",
                frame,
                pass_name,
                material_replaced ? "BASE_S0" : "STOCK_PS"
            );

            if((++log_flush_counter & 31u) == 0u) {
                std::fflush(pass_log);
            }
        }

        static bool compile_base_s0_shader(IDirect3DDevice9 *device) noexcept {
            if(base_s0_shader) {
                return true;
            }
            if(!device) {
                return false;
            }

            ID3DBlob *bytecode = nullptr;
            ID3DBlob *errors = nullptr;
            const HRESULT compile_result = D3DCompile(
                base_s0_hlsl,
                std::strlen(base_s0_hlsl),
                "chimera_d3d9_model_material_diag",
                nullptr,
                nullptr,
                "main",
                "ps_2_0",
                D3DCOMPILE_OPTIMIZATION_LEVEL3,
                0,
                &bytecode,
                &errors
            );

            if(FAILED(compile_result) || !bytecode) {
                if(errors && errors->GetBufferPointer()) {
                    console_output(
                        "D3D9 material diagnostic PS compile failed: %s",
                        static_cast<const char *>(errors->GetBufferPointer())
                    );
                }
                else {
                    console_output(
                        "D3D9 material diagnostic PS compile failed, HRESULT=0x%08lX.",
                        static_cast<unsigned long>(compile_result)
                    );
                }
                if(errors) errors->Release();
                if(bytecode) bytecode->Release();
                return false;
            }

            const HRESULT create_result = device->CreatePixelShader(
                static_cast<const DWORD *>(bytecode->GetBufferPointer()),
                &base_s0_shader
            );
            if(errors) errors->Release();
            bytecode->Release();

            if(FAILED(create_result) || !base_s0_shader) {
                if(base_s0_shader) {
                    base_s0_shader->Release();
                    base_s0_shader = nullptr;
                }
                console_output(
                    "D3D9 material diagnostic CreatePixelShader failed, HRESULT=0x%08lX.",
                    static_cast<unsigned long>(create_result)
                );
                return false;
            }

            console_output("D3D9 backend: compiled base-texture-only ps_2_0 diagnostic.");
            return true;
        }

        static HRESULT STDMETHODCALLTYPE set_pixel_shader_hook(
            IDirect3DDevice9 *device,
            IDirect3DPixelShader9 *shader
        ) {
            if(!original_set_pixel_shader) {
                return D3DERR_INVALIDCALL;
            }

            if(!enabled() || installed_device != device || !base_s0_shader) {
                return original_set_pixel_shader(device, shader);
            }

            IDirect3DVertexShader9 *current_vertex_shader = nullptr;
            if(FAILED(device->GetVertexShader(&current_vertex_shader)) || !current_vertex_shader) {
                return original_set_pixel_shader(device, shader);
            }

            bool primary = false;
            const char *pass_name = classify_vertex_shader(current_vertex_shader, primary);
            current_vertex_shader->Release();

            if(primary) {
                shader = base_s0_shader;
                if(!replacement_announced) {
                    console_output(
                        "D3D9 material diagnostic: primary model passes now sample only texture s0 with TEXCOORD1.xy."
                    );
                    replacement_announced = true;
                }
            }

            log_pass(pass_name, primary);
            return original_set_pixel_shader(device, shader);
        }

        static bool install(IDirect3DDevice9 *device) noexcept {
            if(!device || !enabled()) {
                return false;
            }
            if(!compile_base_s0_shader(device)) {
                return false;
            }

            auto *vtable = *reinterpret_cast<ULONG_PTR **>(device);
            if(!vtable) {
                return false;
            }

            auto *entry = &vtable[DEVICE_SET_PIXEL_SHADER];
            const ULONG_PTR replacement = reinterpret_cast<ULONG_PTR>(set_pixel_shader_hook);
            if(*entry == replacement && installed_device == device && original_set_pixel_shader) {
                return true;
            }

            original_set_pixel_shader = reinterpret_cast<SetPixelShaderFunction>(*entry);
            if(!original_set_pixel_shader) {
                return false;
            }

            DWORD old_protection = 0;
            if(!VirtualProtect(entry, sizeof(*entry), PAGE_EXECUTE_READWRITE, &old_protection)) {
                original_set_pixel_shader = nullptr;
                return false;
            }

            *entry = replacement;
            DWORD ignored = 0;
            VirtualProtect(entry, sizeof(*entry), old_protection, &ignored);
            FlushInstructionCache(GetCurrentProcess(), entry, sizeof(*entry));
            installed_device = device;
            ensure_pass_log();

            if(!installed_announced) {
                console_output(
                    "D3D9 backend: model material/base-texture diagnostic enabled on D3D9On12."
                );
                console_output(
                    "D3D9 material diagnostic: model pass trace -> chimera_d3d9_model_pass.log."
                );
                installed_announced = true;
            }
            return true;
        }

        static void on_end_scene(IDirect3DDevice9 *device) noexcept {
            if(enabled()) {
                install(device);
            }
        }

        static void set_up() noexcept {
            if(!enabled()) {
                return;
            }

            if(!queued_announced) {
                console_output(
                    "D3D9 backend: base-texture model material diagnostic requested; waiting for live D3D9 device."
                );
                queued_announced = true;
            }

            if(!end_scene_retry_registered) {
                add_d3d9_end_scene_event(on_end_scene);
                end_scene_retry_registered = true;
            }

            if(global_d3d9_device && *global_d3d9_device) {
                install(*global_d3d9_device);
            }
        }
    }

    inline void set_up_d3d9_model_material_diag() noexcept {
        D3D9ModelMaterialDiag::set_up();
    }
}

#endif
