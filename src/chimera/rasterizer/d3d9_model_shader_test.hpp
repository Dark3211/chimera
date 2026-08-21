// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_D3D9_MODEL_SHADER_TEST_HPP
#define CHIMERA_D3D9_MODEL_SHADER_TEST_HPP

#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../chimera.hpp"
#include "../config/ini.hpp"
#include "../event/d3d9_end_scene.hpp"
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
            MODEL_FAMILY_TO_SCENERY,
            FIXED_TWO_BONE,
            INDEXED_ONE_BONE,
            MODERN_MULTIBONE
        };

        static SetVertexShaderFunction original_set_vertex_shader = nullptr;
        static IDirect3DDevice9 *installed_device = nullptr;
        static IDirect3DVertexShader9 *diagnostic_shader = nullptr;
        static Mode diagnostic_shader_mode = Mode::OFF;
        static bool announced = false;
        static bool queued_announced = false;
        static bool end_scene_retry_registered = false;
        static std::uint32_t family_hit_mask = 0;
        static std::uint32_t diagnostic_hit_mask = 0;

        static constexpr const char *diagnostic_hlsl = R"HLSL(
float4x4 c_world_view_projection : register(c0);
float4 c_eye_forward : register(c5);
float4 c_fog_screen_gradient : register(c10);
float4 c_base_map_xform_x : register(c11);
float4 c_base_map_xform_y : register(c12);
float4 c_detail_normal_scales : register(c15);
float4x3 c_node_matrices[22] : register(c29);

struct VS_INPUT {
    float4 Position : POSITION0;
    float4 Normal : NORMAL0;
    float4 BiNormal : BINORMAL0;
    float4 Tangent : TANGENT0;
    float4 TexCoord0 : TEXCOORD0;
    float4 BlendIndices : BLENDINDICES0;
    float4 BlendWeights : BLENDWEIGHT0;
};

struct VS_OUTPUT {
    float4 Position : POSITION0;
    float Fog : FOG;
    float3 Position3D : TEXCOORD0;
    float4 DiffuseMultiUV : TEXCOORD1;
    float4 DetailUV : TEXCOORD2;
    float3x3 TBNTranspose : TEXCOORD3;
    float4 NormalDetailUV : TEXCOORD6;
};

VS_OUTPUT main(VS_INPUT IN) {
    VS_OUTPUT OUT = (VS_OUTPUT)0;
    float4x3 WorldMatrix;

#if defined(CHIMERA_FIXED_TWO_BONE)
    // A: weights remain, BlendIndices and relative constant addressing are removed.
    WorldMatrix = c_node_matrices[0] * IN.BlendWeights.x;
    WorldMatrix += c_node_matrices[1] * IN.BlendWeights.y;
#elif defined(CHIMERA_INDEXED_ONE_BONE)
    // B: BlendIndices + dynamic matrix addressing remain; BlendWeights are removed.
    float2 Indices = IN.BlendIndices.xy + c_eye_forward.ww;
    int NodeIndex = (int)Indices.x;
    WorldMatrix = c_node_matrices[NodeIndex];
#elif defined(CHIMERA_MODERN_MULTIBONE)
    // C: modern vs_3_0 equivalent of Halo's stock two-bone GPU skinning path.
    float2 Indices = IN.BlendIndices.xy + c_eye_forward.ww;
    int NodeIndex0 = (int)Indices.x;
    int NodeIndex1 = (int)Indices.y;
    WorldMatrix = c_node_matrices[NodeIndex0] * IN.BlendWeights.x;
    WorldMatrix += c_node_matrices[NodeIndex1] * IN.BlendWeights.y;
#else
    WorldMatrix = c_node_matrices[0];
#endif

    float4 WorldPosition;
    WorldPosition.xyz = mul(IN.Position, WorldMatrix);
    WorldPosition.w = IN.TexCoord0.w;
    OUT.Position3D = WorldPosition.xyz;

    float3x3 WorldToTangentSpace;
    WorldToTangentSpace[0] = mul(IN.Tangent.xyz, (float3x3)WorldMatrix);
    WorldToTangentSpace[1] = mul(IN.BiNormal.xyz, (float3x3)WorldMatrix);
    WorldToTangentSpace[2] = mul(IN.Normal.xyz, (float3x3)WorldMatrix) * c_detail_normal_scales.w;
    OUT.TBNTranspose = transpose(WorldToTangentSpace);

    OUT.Position = mul(WorldPosition, c_world_view_projection);
    OUT.DiffuseMultiUV.x = dot(IN.TexCoord0, c_base_map_xform_x);
    OUT.DiffuseMultiUV.y = dot(IN.TexCoord0, c_base_map_xform_y);
    OUT.DiffuseMultiUV.zw = OUT.DiffuseMultiUV.xy;
    OUT.DetailUV.xy = OUT.DiffuseMultiUV.xy * c_fog_screen_gradient.xy;
    OUT.DetailUV.z = 0.0f;
    OUT.DetailUV.w = c_base_map_xform_y.z;
    OUT.NormalDetailUV.xy = OUT.DiffuseMultiUV.xy * c_detail_normal_scales.xy;
    OUT.NormalDetailUV.zw = OUT.DiffuseMultiUV.xy * c_detail_normal_scales.zw;
    return OUT;
}
)HLSL";

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
            if(_stricmp(value, "fixed_two_bone") == 0) {
                return Mode::FIXED_TWO_BONE;
            }
            if(_stricmp(value, "indexed_one_bone") == 0) {
                return Mode::INDEXED_ONE_BONE;
            }
            if(_stricmp(value, "modern_multibone") == 0) {
                return Mode::MODERN_MULTIBONE;
            }
            return Mode::OFF;
        }

        static bool enabled() noexcept {
            return mode() != Mode::OFF;
        }

        static bool uses_compiled_diagnostic_shader(Mode current_mode) noexcept {
            return current_mode == Mode::FIXED_TWO_BONE
                || current_mode == Mode::INDEXED_ONE_BONE
                || current_mode == Mode::MODERN_MULTIBONE;
        }

        static const char *mode_name(Mode current_mode) noexcept {
            switch(current_mode) {
                case Mode::FIXED_TWO_BONE: return "fixed_two_bone";
                case Mode::INDEXED_ONE_BONE: return "indexed_one_bone";
                case Mode::MODERN_MULTIBONE: return "modern_multibone";
                case Mode::FAST_TO_MODEL: return "fast_to_model";
                case Mode::FAST_TO_SCENERY: return "fast_to_scenery";
                case Mode::MODEL_FAMILY_TO_SCENERY: return "model_family_to_scenery";
                case Mode::OFF:
                default: return "off";
            }
        }

        static void release_diagnostic_shader() noexcept {
            if(diagnostic_shader) {
                diagnostic_shader->Release();
                diagnostic_shader = nullptr;
            }
            diagnostic_shader_mode = Mode::OFF;
            diagnostic_hit_mask = 0;
        }

        static bool create_diagnostic_shader(IDirect3DDevice9 *device, Mode current_mode) noexcept {
            if(!uses_compiled_diagnostic_shader(current_mode)) {
                return true;
            }
            if(diagnostic_shader && diagnostic_shader_mode == current_mode && installed_device == device) {
                return true;
            }

            release_diagnostic_shader();

            D3D_SHADER_MACRO defines[2] = {};
            switch(current_mode) {
                case Mode::FIXED_TWO_BONE:
                    defines[0] = {"CHIMERA_FIXED_TWO_BONE", "1"};
                    break;
                case Mode::INDEXED_ONE_BONE:
                    defines[0] = {"CHIMERA_INDEXED_ONE_BONE", "1"};
                    break;
                case Mode::MODERN_MULTIBONE:
                    defines[0] = {"CHIMERA_MODERN_MULTIBONE", "1"};
                    break;
                default:
                    return false;
            }

            ID3DBlob *bytecode = nullptr;
            ID3DBlob *errors = nullptr;
            const HRESULT compile_result = D3DCompile(
                diagnostic_hlsl,
                std::strlen(diagnostic_hlsl),
                "chimera_d3d9_model_shader_test",
                defines,
                nullptr,
                "main",
                "vs_3_0",
                D3DCOMPILE_OPTIMIZATION_LEVEL3,
                0,
                &bytecode,
                &errors
            );

            if(FAILED(compile_result) || !bytecode) {
                if(errors && errors->GetBufferPointer()) {
                    console_output(
                        "D3D9 model shader diagnostic compile failed (%s): %s",
                        mode_name(current_mode),
                        static_cast<const char *>(errors->GetBufferPointer())
                    );
                }
                else {
                    console_output(
                        "D3D9 model shader diagnostic compile failed (%s), HRESULT=0x%08lX.",
                        mode_name(current_mode),
                        static_cast<unsigned long>(compile_result)
                    );
                }
                if(errors) errors->Release();
                if(bytecode) bytecode->Release();
                return false;
            }

            const HRESULT create_result = device->CreateVertexShader(
                static_cast<const DWORD *>(bytecode->GetBufferPointer()),
                &diagnostic_shader
            );
            if(errors) errors->Release();
            bytecode->Release();

            if(FAILED(create_result) || !diagnostic_shader) {
                diagnostic_shader = nullptr;
                console_output(
                    "D3D9 model shader diagnostic CreateVertexShader failed (%s), HRESULT=0x%08lX.",
                    mode_name(current_mode),
                    static_cast<unsigned long>(create_result)
                );
                return false;
            }

            diagnostic_shader_mode = current_mode;
            console_output(
                "D3D9 backend: compiled modern vs_3_0 model diagnostic '%s'.",
                mode_name(current_mode)
            );
            return true;
        }

        static void announce_family_hit(std::uint32_t bit, const char *name) noexcept {
            if(family_hit_mask & bit) return;
            family_hit_mask |= bit;
            console_output("D3D9 model shader isolation: replacing %s with VSH_MODEL_SCENERY.", name);
        }

        static bool substitute_model_family_shader(IDirect3DVertexShader9 *&shader) noexcept {
            if(!vertex_shaders || !vertex_shaders[VSH_MODEL_SCENERY].shader) return false;

            struct Candidate {
                VertexShaderIndex index;
                std::uint32_t bit;
                const char *name;
            };
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

        static bool substitute_with_diagnostic_shader(IDirect3DVertexShader9 *&shader) noexcept {
            if(!diagnostic_shader || !vertex_shaders) return false;

            struct Candidate {
                VertexShaderIndex index;
                std::uint32_t bit;
                const char *name;
            };
            static constexpr Candidate candidates[] = {
                {VSH_MODEL,      1u << 0, "VSH_MODEL"},
                {VSH_MODEL_FAST, 1u << 1, "VSH_MODEL_FAST"},
            };

            for(const auto &candidate : candidates) {
                if(vertex_shaders[candidate.index].shader
                    && shader == vertex_shaders[candidate.index].shader) {
                    if(!(diagnostic_hit_mask & candidate.bit)) {
                        diagnostic_hit_mask |= candidate.bit;
                        console_output(
                            "D3D9 model shader diagnostic: replacing %s with %s.",
                            candidate.name,
                            mode_name(diagnostic_shader_mode)
                        );
                    }
                    shader = diagnostic_shader;
                    return true;
                }
            }
            return false;
        }

        static HRESULT STDMETHODCALLTYPE set_vertex_shader_hook(
            IDirect3DDevice9 *device,
            IDirect3DVertexShader9 *shader
        ) {
            if(!original_set_vertex_shader) return D3DERR_INVALIDCALL;

            const Mode current_mode = mode();
            switch(current_mode) {
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
                        shader = vertex_shaders[VSH_MODEL_SCENERY].shader;
                    }
                    break;
                case Mode::MODEL_FAMILY_TO_SCENERY:
                    substitute_model_family_shader(shader);
                    break;
                case Mode::FIXED_TWO_BONE:
                case Mode::INDEXED_ONE_BONE:
                case Mode::MODERN_MULTIBONE:
                    substitute_with_diagnostic_shader(shader);
                    break;
                case Mode::OFF:
                    break;
            }
            return original_set_vertex_shader(device, shader);
        }

        static bool install(IDirect3DDevice9 *device) noexcept {
            if(!device || !enabled()) return false;

            const Mode current_mode = mode();
            if(!create_diagnostic_shader(device, current_mode)) return false;

            auto *vtable = *reinterpret_cast<ULONG_PTR **>(device);
            if(!vtable) return false;

            auto *entry = &vtable[DEVICE_SET_VERTEX_SHADER];
            const ULONG_PTR replacement = reinterpret_cast<ULONG_PTR>(set_vertex_shader_hook);
            if(*entry == replacement && installed_device == device && original_set_vertex_shader) return true;

            original_set_vertex_shader = reinterpret_cast<SetVertexShaderFunction>(*entry);
            if(!original_set_vertex_shader) return false;

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
                switch(current_mode) {
                    case Mode::FAST_TO_SCENERY:
                        console_output("D3D9 backend: testing VSH_MODEL_FAST -> VSH_MODEL_SCENERY single-bone isolation on D3D9On12.");
                        break;
                    case Mode::MODEL_FAMILY_TO_SCENERY:
                        console_output("D3D9 backend: testing visible model shader family -> VSH_MODEL_SCENERY single-bone isolation on D3D9On12.");
                        break;
                    case Mode::FAST_TO_MODEL:
                        console_output("D3D9 backend: testing VSH_MODEL_FAST -> VSH_MODEL substitution on D3D9On12.");
                        break;
                    case Mode::FIXED_TWO_BONE:
                        console_output("D3D9 backend: testing modern fixed-node two-bone shader (weights yes, indices no).");
                        break;
                    case Mode::INDEXED_ONE_BONE:
                        console_output("D3D9 backend: testing modern indexed single-bone shader (indices yes, weights no).");
                        break;
                    case Mode::MODERN_MULTIBONE:
                        console_output("D3D9 backend: testing modern vs_3_0 equivalent of Halo two-bone model skinning.");
                        break;
                    case Mode::OFF:
                        break;
                }
                announced = true;
            }
            return true;
        }

        static void on_end_scene(IDirect3DDevice9 *device) noexcept {
            if(enabled()) install(device);
        }

        static void set_up() noexcept {
            if(!enabled()) return;

            if(!queued_announced) {
                console_output(
                    "D3D9 backend: model shader isolation mode '%s' recognized; waiting for live D3D9 device.",
                    mode_name(mode())
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

    inline void set_up_d3d9_model_shader_test() noexcept {
        D3D9ModelShaderTest::set_up();
    }
}

#endif
