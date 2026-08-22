// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_D3D9_MODEL_SHADER_PRIMARY_V2_HPP
#define CHIMERA_D3D9_MODEL_SHADER_PRIMARY_V2_HPP

#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "rasterizer_vertex_shaders.hpp"
#include "../chimera.hpp"
#include "../config/ini.hpp"
#include "../event/d3d9_end_scene.hpp"
#include "../halo_data/game_variables.hpp"
#include "../halo_data/shader_effects.hpp"
#include "../output/output.hpp"

namespace Chimera {
    namespace D3D9ModelShaderPrimaryV2 {
        constexpr std::size_t DEVICE_SET_VERTEX_SHADER = 92;

        using SetVertexShaderFunction = HRESULT (STDMETHODCALLTYPE *)(
            IDirect3DDevice9 *, IDirect3DVertexShader9 *
        );

        static SetVertexShaderFunction original_set_vertex_shader = nullptr;
        static IDirect3DDevice9 *installed_device = nullptr;
        static IDirect3DVertexShader9 *model_shader = nullptr;
        static IDirect3DVertexShader9 *fogged_shader = nullptr;
        static IDirect3DVertexShader9 *active_camo_shader = nullptr;
        static IDirect3DVertexShader9 *shadow_shader = nullptr;
        static bool queued_announced = false;
        static bool installed_announced = false;
        static bool end_scene_retry_registered = false;
        static std::uint32_t hit_mask = 0;

        // MODEL / MODEL_FAST / MODEL_FOGGED use Halo's ModelVS path, including
        // the +0.5 node-index convention. Active camouflage and shadow use the
        // older GetPosition helper and therefore consume the raw node indices.
        // Keeping both conventions separate is important for matching the stock
        // Halo CE shaders on D3D9On12.
        static constexpr const char *primary_hlsl = R"HLSL(
float4x4 c_world_view_projection : register(c0);
float4 c_eye_position : register(c4);
float4 c_eye_forward : register(c5);
float4 c_planar_fog_gradient1 : register(c6);
float4 c_planar_fog_gradient2 : register(c7);
float4 c_fog_densities : register(c9);
float4 c_fog_screen_gradient : register(c10);
float4 c_base_map_xform_x : register(c11);
float4 c_base_map_xform_y : register(c12);
float4 c_screenproj_0 : register(c13);
float4 c_screenproj_1 : register(c14);
float4 c_screenproj_2 : register(c15);
float4 c_screenproj_3 : register(c16);
#define c_detail_normal_scales c_screenproj_2
float4 c_eye_xform_x : register(c27);
float4 c_eye_xform_y : register(c28);
float4x3 c_node_matrices[22] : register(c29);

struct VS_INPUT {
    float4 Position : POSITION0;
    float3 Normal : NORMAL0;
    float3 BiNormal : BINORMAL0;
    float3 Tangent : TANGENT0;
    float4 TexCoord0 : TEXCOORD0;
    float2 BlendIndices : BLENDINDICES0;
    float2 BlendWeights : BLENDWEIGHT0;
};

struct MODEL_OUTPUT {
    float4 Position : POSITION0;
    float Fog : FOG;
    float4 T0 : TEXCOORD0;
    float4 T1 : TEXCOORD1;
    float4 T2 : TEXCOORD2;
    float4 T3 : TEXCOORD3;
    float4 T4 : TEXCOORD4;
    float4 T5 : TEXCOORD5;
    float4 T6 : TEXCOORD6;
};

float4x3 GetWorldMatrixModel(VS_INPUT IN) {
    float2 Indices = IN.BlendIndices + c_eye_forward.ww;
    int NodeIndex0 = (int)Indices.x;
    int NodeIndex1 = (int)Indices.y;
    float4x3 WorldMatrix = c_node_matrices[NodeIndex0] * IN.BlendWeights.x;
    WorldMatrix += c_node_matrices[NodeIndex1] * IN.BlendWeights.y;
    return WorldMatrix;
}

float4x3 GetWorldMatrixRaw(VS_INPUT IN) {
    int NodeIndex0 = (int)IN.BlendIndices.x;
    int NodeIndex1 = (int)IN.BlendIndices.y;
    float4x3 WorldMatrix = c_node_matrices[NodeIndex0] * IN.BlendWeights.x;
    WorldMatrix += c_node_matrices[NodeIndex1] * IN.BlendWeights.y;
    return WorldMatrix;
}

float CalculatePlanarFog(float3 WorldPosition) {
    float PlanarFogVertexDensity = dot(WorldPosition, c_planar_fog_gradient1.xyz) + c_planar_fog_gradient1.w;
    float PlanarFogEyeDistance = dot(WorldPosition, c_planar_fog_gradient2.xyz) + c_planar_fog_gradient2.w;
    float2 FogDensity = 1.0f - float2(PlanarFogVertexDensity, PlanarFogEyeDistance);
    FogDensity = max(FogDensity, 0.0f);
    FogDensity *= FogDensity;
    FogDensity = min(FogDensity, 1.0f);
    FogDensity.x = min(FogDensity.x + FogDensity.y, 1.0f);
    FogDensity = 1.0f - FogDensity;
    FogDensity *= FogDensity;
    FogDensity.y -= FogDensity.x;
    return (c_fog_densities.y * FogDensity.y + FogDensity.x) * c_fog_densities.z;
}

MODEL_OUTPUT BuildModel(VS_INPUT IN, bool Fogged) {
    MODEL_OUTPUT OUT = (MODEL_OUTPUT)0;
    float4x3 WorldMatrix = GetWorldMatrixModel(IN);

    float4 WorldPosition;
    WorldPosition.xyz = mul(IN.Position, WorldMatrix);
    WorldPosition.w = IN.TexCoord0.w;

    OUT.Position = mul(WorldPosition, c_world_view_projection);
    OUT.T0 = float4(WorldPosition.xyz, 0.0f);

    OUT.T1.x = dot(IN.TexCoord0, c_base_map_xform_x);
    OUT.T1.y = dot(IN.TexCoord0, c_base_map_xform_y);
    OUT.T1.zw = OUT.T1.xy;

    OUT.T2.xy = OUT.T1.xy * c_fog_screen_gradient.xy;
    OUT.T2.z = 0.0f;
    OUT.T2.w = c_base_map_xform_y.z;

    float3 Tangent = mul(IN.Tangent, (float3x3)WorldMatrix);
    float3 BiNormal = mul(IN.BiNormal, (float3x3)WorldMatrix);
    float3 Normal = mul(IN.Normal, (float3x3)WorldMatrix) * c_fog_screen_gradient.w;

    OUT.T3 = float4(Tangent.x, BiNormal.x, Normal.x, 0.0f);
    OUT.T4 = float4(Tangent.y, BiNormal.y, Normal.y, 0.0f);
    OUT.T5 = float4(Tangent.z, BiNormal.z, Normal.z, 0.0f);

    OUT.T6.xy = OUT.T1.xy * c_detail_normal_scales.xy;
    OUT.T6.zw = OUT.T1.xy * c_detail_normal_scales.zw;

    if(Fogged) {
        float FogDensity = CalculatePlanarFog(WorldPosition.xyz);
        OUT.T2.z = FogDensity;
        OUT.Fog = IN.TexCoord0.w - FogDensity;
    }
    return OUT;
}

MODEL_OUTPUT main_model(VS_INPUT IN) {
    return BuildModel(IN, false);
}

MODEL_OUTPUT main_fogged(VS_INPUT IN) {
    return BuildModel(IN, true);
}

struct CAMO_OUTPUT {
    float4 Position : POSITION0;
    float4 D0 : COLOR0;
    float3 T0 : TEXCOORD0;
    float3 T1 : TEXCOORD1;
    float3 T2 : TEXCOORD2;
};

CAMO_OUTPUT main_active_camo(VS_INPUT IN) {
    CAMO_OUTPUT OUT = (CAMO_OUTPUT)0;
    float4x3 WorldMatrix = GetWorldMatrixRaw(IN);
    float3 WorldPosition3 = mul(IN.Position, WorldMatrix);
    float4 WorldPosition = float4(WorldPosition3, 1.0f);
    float3 WorldNormal = normalize(mul(IN.Normal, (float3x3)WorldMatrix));

    float3 EyeVector = c_eye_position.xyz - WorldPosition3;
    float EyeDistance = dot(EyeVector, c_eye_forward.xyz);
    float CamoFactor = saturate((1.0f / (EyeDistance * EyeDistance)) * c_fog_screen_gradient.y);

    OUT.D0.xyz = c_base_map_xform_y.xyz;
    OUT.D0.w = CamoFactor;
    OUT.Position = mul(WorldPosition, c_world_view_projection);

    OUT.T0.x = dot(WorldNormal, c_eye_xform_x.xyz);
    OUT.T0.y = dot(WorldNormal, c_eye_xform_y.xyz);
    OUT.T0.z = dot(WorldNormal, c_eye_forward.xyz);

    OUT.T1.x = CamoFactor * c_fog_screen_gradient.x;
    OUT.T1.y = 0.0f;
    OUT.T2.x = 0.0f;
    OUT.T2.y = CamoFactor * c_fog_screen_gradient.x;

    float2 Projected = OUT.Position.xy / OUT.Position.w;
    Projected = (Projected + float2(1.0f, -1.0f)) * 0.5f;
    Projected.y *= -1.0f;
    OUT.T1.z = c_fog_screen_gradient.z * Projected.x;
    OUT.T2.z = c_fog_screen_gradient.w * Projected.y;
    return OUT;
}

struct SHADOW_OUTPUT {
    float4 Position : POSITION0;
    float2 T0 : TEXCOORD0;
};

SHADOW_OUTPUT main_shadow(VS_INPUT IN) {
    SHADOW_OUTPUT OUT = (SHADOW_OUTPUT)0;
    float4x3 WorldMatrix = GetWorldMatrixRaw(IN);
    float4 WorldPosition = float4(mul(IN.Position, WorldMatrix), 1.0f);
    OUT.Position = float4(
        dot(WorldPosition, c_screenproj_0),
        dot(WorldPosition, c_screenproj_1),
        dot(WorldPosition, c_screenproj_2),
        dot(WorldPosition, c_screenproj_3)
    );
    OUT.T0.x = dot(IN.TexCoord0, c_base_map_xform_x);
    OUT.T0.y = dot(IN.TexCoord0, c_base_map_xform_y);
    return OUT;
}
)HLSL";

        static bool d3d9on12_requested() noexcept {
            auto *backend = get_chimera().get_ini()->get_value("video_mode.d3d_backend");
            return backend
                && (_stricmp(backend, "9on12") == 0 || _stricmp(backend, "d3d9on12") == 0);
        }

        static bool enabled() noexcept {
            if(!d3d9on12_requested()) {
                return false;
            }
            auto *value = get_chimera().get_ini()->get_value("video_mode.d3d_model_shader_compat");
            return value && (_stricmp(value, "vs2_primary_v2") == 0 || _stricmp(value, "primary_v2") == 0);
        }

        static void release_shaders() noexcept {
            if(model_shader) {
                model_shader->Release();
                model_shader = nullptr;
            }
            if(fogged_shader) {
                fogged_shader->Release();
                fogged_shader = nullptr;
            }
            if(active_camo_shader) {
                active_camo_shader->Release();
                active_camo_shader = nullptr;
            }
            if(shadow_shader) {
                shadow_shader->Release();
                shadow_shader = nullptr;
            }
            installed_device = nullptr;
            hit_mask = 0;
        }

        static bool compile_shader(IDirect3DDevice9 *device, const char *entry, IDirect3DVertexShader9 **out) noexcept {
            if(!device || !entry || !out) {
                return false;
            }
            *out = nullptr;

            ID3DBlob *bytecode = nullptr;
            ID3DBlob *errors = nullptr;
            const HRESULT compile_result = D3DCompile(
                primary_hlsl,
                std::strlen(primary_hlsl),
                "chimera_d3d9_model_shader_primary_v2",
                nullptr,
                nullptr,
                entry,
                "vs_2_0",
                D3DCOMPILE_OPTIMIZATION_LEVEL3,
                0,
                &bytecode,
                &errors
            );

            if(FAILED(compile_result) || !bytecode) {
                if(errors && errors->GetBufferPointer()) {
                    console_output("D3D9 primary VS2 compile failed (%s): %s", entry, static_cast<const char *>(errors->GetBufferPointer()));
                }
                else {
                    console_output("D3D9 primary VS2 compile failed (%s), HRESULT=0x%08lX.", entry, static_cast<unsigned long>(compile_result));
                }
                if(errors) errors->Release();
                if(bytecode) bytecode->Release();
                return false;
            }

            const HRESULT create_result = device->CreateVertexShader(
                static_cast<const DWORD *>(bytecode->GetBufferPointer()), out
            );
            if(errors) errors->Release();
            bytecode->Release();

            if(FAILED(create_result) || !*out) {
                if(*out) {
                    (*out)->Release();
                    *out = nullptr;
                }
                console_output("D3D9 primary VS2 CreateVertexShader failed (%s), HRESULT=0x%08lX.", entry, static_cast<unsigned long>(create_result));
                return false;
            }
            return true;
        }

        static bool ensure_shaders(IDirect3DDevice9 *device) noexcept {
            if(installed_device == device && model_shader && fogged_shader && active_camo_shader && shadow_shader) {
                return true;
            }

            release_shaders();
            if(!compile_shader(device, "main_model", &model_shader)
                || !compile_shader(device, "main_fogged", &fogged_shader)
                || !compile_shader(device, "main_active_camo", &active_camo_shader)
                || !compile_shader(device, "main_shadow", &shadow_shader)) {
                release_shaders();
                return false;
            }
            installed_device = device;
            console_output("D3D9 backend: compiled VS2 model, fogged, active-camo and shadow compatibility shaders.");
            return true;
        }

        static void announce_hit(std::uint32_t bit, const char *name) noexcept {
            if(hit_mask & bit) {
                return;
            }
            hit_mask |= bit;
            console_output("D3D9 primary VS2 v2: replacing %s.", name);
        }

        static HRESULT STDMETHODCALLTYPE set_vertex_shader_hook(
            IDirect3DDevice9 *device,
            IDirect3DVertexShader9 *shader
        ) {
            if(!original_set_vertex_shader) {
                return D3DERR_INVALIDCALL;
            }

            if(enabled() && vertex_shaders && installed_device == device) {
                // Live A/B path: replace the complete Halo MODEL shader family
                // with the stock-equivalent VS3 bank. This takes priority over
                // primary_v2 only while explicitly enabled by the console
                // command; turning it off restores the already-proven path.
                if(rasterizer_modern_model_test_enabled()) {
                    for(std::uint16_t i = VSH_MODEL_FOGGED; i <= VSH_MODEL_ZBUFFER; i++) {
                        IDirect3DVertexShader9 *stock = vertex_shaders[i].shader;
                        if(stock && shader == stock) {
                            if(IDirect3DVertexShader9 *modern = rasterizer_get_modern_vertex_shader(i)) {
                                shader = modern;
                            }
                            break;
                        }
                    }
                }
                else if(vertex_shaders[VSH_MODEL_FOGGED].shader && shader == vertex_shaders[VSH_MODEL_FOGGED].shader) {
                    announce_hit(1u << 0, "VSH_MODEL_FOGGED");
                    shader = fogged_shader;
                }
                else if(vertex_shaders[VSH_MODEL].shader && shader == vertex_shaders[VSH_MODEL].shader) {
                    announce_hit(1u << 1, "VSH_MODEL");
                    shader = model_shader;
                }
                else if(vertex_shaders[VSH_MODEL_FAST].shader && shader == vertex_shaders[VSH_MODEL_FAST].shader) {
                    announce_hit(1u << 2, "VSH_MODEL_FAST");
                    shader = model_shader;
                }
                else if(vertex_shaders[VSH_MODEL_ACTIVE_CAMOUFLAGE].shader && shader == vertex_shaders[VSH_MODEL_ACTIVE_CAMOUFLAGE].shader) {
                    announce_hit(1u << 3, "VSH_MODEL_ACTIVE_CAMOUFLAGE");
                    shader = active_camo_shader;
                }
                else if(vertex_shaders[VSH_MODEL_SHADOW].shader && shader == vertex_shaders[VSH_MODEL_SHADOW].shader) {
                    announce_hit(1u << 4, "VSH_MODEL_SHADOW");
                    shader = shadow_shader;
                }
            }

            return original_set_vertex_shader(device, shader);
        }

        static bool install(IDirect3DDevice9 *device) noexcept {
            if(!device || !enabled()) {
                return false;
            }
            if(!ensure_shaders(device)) {
                return false;
            }

            auto *vtable = *reinterpret_cast<ULONG_PTR **>(device);
            if(!vtable) {
                return false;
            }
            auto *entry = &vtable[DEVICE_SET_VERTEX_SHADER];
            const ULONG_PTR replacement = reinterpret_cast<ULONG_PTR>(set_vertex_shader_hook);
            if(*entry == replacement && original_set_vertex_shader) {
                return true;
            }

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

            if(!installed_announced) {
                console_output("D3D9 backend: explicit-output primary VS2 compatibility enabled on D3D9On12.");
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
                console_output("D3D9 backend: explicit-output primary VS2 test requested; waiting for live D3D9 device.");
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

    inline void set_up_d3d9_model_shader_primary_v2() noexcept {
        D3D9ModelShaderPrimaryV2::set_up();
    }
}

#endif
