// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_D3D9_MODEL_SAFE_LIGHTING_VS3_HPP
#define CHIMERA_D3D9_MODEL_SAFE_LIGHTING_VS3_HPP

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
    namespace D3D9ModelSafeLightingVS3 {
        constexpr std::size_t DEVICE_SET_VERTEX_SHADER = 92;

        using SetVertexShaderFunction = HRESULT (STDMETHODCALLTYPE *)(
            IDirect3DDevice9 *, IDirect3DVertexShader9 *
        );

        static SetVertexShaderFunction original_set_vertex_shader = nullptr;
        static IDirect3DDevice9 *installed_device = nullptr;
        static IDirect3DVertexShader9 *model_shader = nullptr;
        static IDirect3DVertexShader9 *fast_shader = nullptr;
        static IDirect3DVertexShader9 *fogged_shader = nullptr;
        static bool end_scene_retry_registered = false;
        static bool safe_mode = false;
        static std::uint32_t hit_mask = 0;

        // Stock MODEL/MODEL_FAST/MODEL_FOGGED output contract, rebuilt as VS3.
        // Geometry uses the same explicit two-node HLSL indexing that proved
        // stable in primary_v2, while vertex colour/lighting follows Halo's
        // stock VS1.1 assembly exactly enough to restore oD0/oD1 without
        // returning to legacy relative-address-register skinning.
        static constexpr const char *safe_model_hlsl = R"HLSL(
float4x4 c_world_view_projection : register(c0);
float4 c_eye_position : register(c4);
float4 c_eye_forward : register(c5);
float4 c_fog_plane : register(c6);
float4 c_planar_fog_gradient1 : register(c7);
float4 c_planar_fog_gradient2 : register(c8);
float4 c_fog_densities : register(c9);
float4 c_model_scale : register(c10);
float4 c_base_map_xform_x : register(c11);
float4 c_base_map_xform_y : register(c12);
float4 c_view_angle_scale : register(c13);
float4 c_view_angle_bias : register(c14);
float4 c_point0_position : register(c15);
float4 c_point0_direction : register(c16);
float4 c_point0_color : register(c17);
float4 c_point1_position : register(c18);
float4 c_point1_direction : register(c19);
float4 c_point1_color : register(c20);
float4 c_direction0 : register(c21);
float4 c_direction0_color : register(c22);
float4 c_direction1 : register(c23);
float4 c_direction1_color : register(c24);
float4 c_ambient : register(c25);
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
    float4 D0 : COLOR0;
    float4 D1 : COLOR1;
    float2 T0 : TEXCOORD0;
    float2 T1 : TEXCOORD1;
    float2 T2 : TEXCOORD2;
    float3 T3 : TEXCOORD3;
    float Fog : FOG;
};

float4x3 GetWorldMatrix(VS_INPUT IN) {
    // Halo's VS1.1 path multiplies the node selector by the three-register
    // matrix stride and then adds c5.w before MOV a0. HLSL matrix-array
    // addressing supplies the stride, so only the stock +c5.w conversion is
    // required here. This is the already-validated primary_v2 convention.
    float2 Indices = IN.BlendIndices + c_eye_forward.ww;
    int NodeIndex0 = (int)Indices.x;
    int NodeIndex1 = (int)Indices.y;
    float4x3 WorldMatrix = c_node_matrices[NodeIndex0] * IN.BlendWeights.x;
    WorldMatrix += c_node_matrices[NodeIndex1] * IN.BlendWeights.y;
    return WorldMatrix;
}

float ClampStock(float Value, float Low, float High) {
    return min(max(Value, Low), High);
}

float3 FullDiffuseLighting(float3 WorldPosition, float3 Normal, float Low, float High) {
    float3 ToPoint0 = c_point0_position.xyz - WorldPosition;
    float DistanceSq0 = dot(ToPoint0, ToPoint0);
    float3 L0 = ToPoint0 * rsqrt(DistanceSq0);

    float3 ToPoint1 = c_point1_position.xyz - WorldPosition;
    float DistanceSq1 = dot(ToPoint1, ToPoint1);
    float3 L1 = ToPoint1 * rsqrt(DistanceSq1);

    // r8/r9 from the stock shader. r8 is only clamped at the low end; r9 is
    // clamped to [v4.z, v4.w]. Keep that asymmetry because it affects Halo's
    // point-light attenuation and directional contribution.
    float Attenuation0 = max(High - DistanceSq0 * c_point0_position.w, Low);
    float Attenuation1 = max(High - DistanceSq1 * c_point1_position.w, Low);
    float Cone0 = max(dot(L0, -c_point0_direction.xyz) * c_point0_direction.w + c_point0_color.w, Low);

    float NDotL0 = ClampStock(dot(L0, Normal), Low, High);
    float NDotL1 = ClampStock(dot(L1, Normal), Low, High);
    float Cone1 = ClampStock(dot(L1, -c_point1_direction.xyz) * c_point1_direction.w + c_point1_color.w, Low, High);

    float Direction0 = dot(Normal, -c_direction0.xyz);
    Direction0 = max(Direction0, -Direction0 * c_base_map_xform_y.z);
    Direction0 = max(Direction0, Low);

    float Direction1 = ClampStock(dot(Normal, -c_direction1.xyz), Low, High);

    float Point0 = Attenuation0 * NDotL0 * Cone0;
    float Point1 = Attenuation1 * NDotL1 * Cone1;

    float3 Diffuse = c_ambient.xyz;
    Diffuse += Point0 * c_point0_color.xyz;
    Diffuse += Point1 * c_point1_color.xyz;
    Diffuse += Direction0 * c_direction0_color.xyz;
    Diffuse += Direction1 * c_direction1_color.xyz;
    return Diffuse;
}

float3 FastDiffuseLighting(float3 Normal, float Low) {
    float Direction0 = dot(Normal, -c_direction0.xyz);
    Direction0 = max(Direction0, -Direction0 * c_base_map_xform_y.z);
    Direction0 = max(Direction0, Low);

    float Direction1 = max(dot(Normal, -c_direction1.xyz), Low);

    float3 Diffuse = c_ambient.xyz;
    Diffuse += Direction0 * c_direction0_color.xyz;
    Diffuse += Direction1 * c_direction1_color.xyz;
    return Diffuse;
}

float4 ViewAngleColor(float3 WorldPosition, float3 Normal) {
    float3 EyeVector = c_eye_position.xyz - WorldPosition;
    float EyeLengthSq = dot(EyeVector, EyeVector);
    float3 EyeDirection = EyeVector * rsqrt(EyeLengthSq);
    float Facing = dot(EyeDirection, Normal);
    return Facing * c_view_angle_scale + c_view_angle_bias;
}

float FoggedDensity(float3 WorldPosition, float Low, float High) {
    float A = dot(WorldPosition, c_planar_fog_gradient1.xyz) + c_planar_fog_gradient1.w;
    float B = dot(WorldPosition, c_planar_fog_gradient2.xyz) + c_planar_fog_gradient2.w;

    float2 Density = High - float2(A, B);
    Density = max(Density, Low);
    Density *= Density;
    Density = min(Density, High);
    Density.x = min(Density.x + Density.y, High);
    Density = High - Density;
    Density *= Density;
    Density.y -= Density.x;
    return (c_fog_densities.y * Density.y + Density.x) * c_fog_densities.z;
}

MODEL_OUTPUT BuildCommon(VS_INPUT IN, int LightingMode, int FogMode) {
    MODEL_OUTPUT OUT = (MODEL_OUTPUT)0;
    float4x3 WorldMatrix = GetWorldMatrix(IN);

    float4 WorldPosition;
    WorldPosition.xyz = mul(IN.Position, WorldMatrix);
    WorldPosition.w = IN.TexCoord0.w;

    float3 Normal = mul(IN.Normal, (float3x3)WorldMatrix) * c_model_scale.w;
    float3 EyeVector = c_eye_position.xyz - WorldPosition.xyz;
    float EyeDotNormal = dot(EyeVector, Normal);
    float3 Reflection = EyeDotNormal * Normal * c_eye_position.w - EyeVector;

    OUT.Position = mul(WorldPosition, c_world_view_projection);

    float2 UV;
    UV.x = dot(IN.TexCoord0, c_base_map_xform_x);
    UV.y = dot(IN.TexCoord0, c_base_map_xform_y);
    OUT.T0 = UV;
    OUT.T1 = UV * c_model_scale.xy;
    OUT.T2 = UV;
    OUT.T3 = Reflection;

    if(LightingMode == 1) {
        OUT.D0.xyz = FastDiffuseLighting(Normal, IN.TexCoord0.z);
    }
    else {
        OUT.D0.xyz = FullDiffuseLighting(
            WorldPosition.xyz,
            Normal,
            IN.TexCoord0.z,
            IN.TexCoord0.w
        );
    }
    OUT.D1 = ViewAngleColor(WorldPosition.xyz, Normal);

    if(FogMode == 1) {
        float Density = FoggedDensity(
            WorldPosition.xyz,
            IN.TexCoord0.z,
            IN.TexCoord0.w
        );
        OUT.D0.w = Density;
        OUT.Fog = IN.TexCoord0.w - Density;
    }
    else if(LightingMode == 1) {
        OUT.D0.w = IN.TexCoord0.z;
        float Plane = dot(WorldPosition.xyz, c_fog_plane.xyz) + c_fog_plane.w;
        OUT.Fog = IN.TexCoord0.w - Plane;
    }
    else {
        OUT.D0.w = IN.TexCoord0.z;
        OUT.Fog = IN.TexCoord0.w - dot(WorldPosition, c_fog_plane);
    }
    return OUT;
}

MODEL_OUTPUT main_model(VS_INPUT IN) {
    return BuildCommon(IN, 0, 0);
}

MODEL_OUTPUT main_fast(VS_INPUT IN) {
    return BuildCommon(IN, 1, 0);
}

MODEL_OUTPUT main_fogged(VS_INPUT IN) {
    return BuildCommon(IN, 0, 1);
}
)HLSL";

        static bool d3d9on12_requested() noexcept {
            auto *backend = get_chimera().get_ini()->get_value("video_mode.d3d_backend");
            return backend
                && (_stricmp(backend, "9on12") == 0 || _stricmp(backend, "d3d9on12") == 0);
        }

        static bool primary_v2_requested() noexcept {
            if(!d3d9on12_requested()) {
                return false;
            }
            auto *value = get_chimera().get_ini()->get_value("video_mode.d3d_model_shader_compat");
            return value && (_stricmp(value, "vs2_primary_v2") == 0 || _stricmp(value, "primary_v2") == 0);
        }

        inline bool safe_mode_enabled() noexcept {
            return safe_mode;
        }

        inline void set_safe_mode(bool enabled) noexcept {
            safe_mode = enabled;
            if(enabled) {
                // Never stack the diagnostic stock-equivalent MODEL bank with
                // the safe-lighting path. Safe mode owns MODEL/Fast/Fogged;
                // primary_v2 continues to own active-camo/shadow.
                rasterizer_set_modern_model_test(false);
            }
            console_output(
                "D3D9 modern MODEL safe-lighting VS3: %s.",
                enabled ? "ON" : "OFF"
            );
        }

        static void release_shaders() noexcept {
            if(model_shader) {
                model_shader->Release();
                model_shader = nullptr;
            }
            if(fast_shader) {
                fast_shader->Release();
                fast_shader = nullptr;
            }
            if(fogged_shader) {
                fogged_shader->Release();
                fogged_shader = nullptr;
            }
            installed_device = nullptr;
            hit_mask = 0;
        }

        static bool compile_shader(
            IDirect3DDevice9 *device,
            const char *entry,
            IDirect3DVertexShader9 **out
        ) noexcept {
            if(!device || !entry || !out) {
                return false;
            }
            *out = nullptr;

            ID3DBlob *bytecode = nullptr;
            ID3DBlob *errors = nullptr;
            const HRESULT compile_result = D3DCompile(
                safe_model_hlsl,
                std::strlen(safe_model_hlsl),
                "chimera_d3d9_model_safe_lighting_vs3",
                nullptr,
                nullptr,
                entry,
                "vs_3_0",
                D3DCOMPILE_OPTIMIZATION_LEVEL3,
                0,
                &bytecode,
                &errors
            );

            if(FAILED(compile_result) || !bytecode) {
                if(errors && errors->GetBufferPointer()) {
                    console_error(
                        "D3D9 safe MODEL VS3 compile failed (%s): %s",
                        entry,
                        static_cast<const char *>(errors->GetBufferPointer())
                    );
                }
                else {
                    console_error(
                        "D3D9 safe MODEL VS3 compile failed (%s), HRESULT=0x%08lX.",
                        entry,
                        static_cast<unsigned long>(compile_result)
                    );
                }
                if(errors) errors->Release();
                if(bytecode) bytecode->Release();
                return false;
            }

            const HRESULT create_result = device->CreateVertexShader(
                static_cast<const DWORD *>(bytecode->GetBufferPointer()),
                out
            );
            if(errors) errors->Release();
            bytecode->Release();

            if(FAILED(create_result) || !*out) {
                if(*out) {
                    (*out)->Release();
                    *out = nullptr;
                }
                console_error(
                    "D3D9 safe MODEL VS3 CreateVertexShader failed (%s), HRESULT=0x%08lX.",
                    entry,
                    static_cast<unsigned long>(create_result)
                );
                return false;
            }
            return true;
        }

        static bool ensure_shaders(IDirect3DDevice9 *device) noexcept {
            if(installed_device == device && model_shader && fast_shader && fogged_shader) {
                return true;
            }

            release_shaders();
            if(!compile_shader(device, "main_model", &model_shader)
                || !compile_shader(device, "main_fast", &fast_shader)
                || !compile_shader(device, "main_fogged", &fogged_shader)) {
                release_shaders();
                return false;
            }
            installed_device = device;
            console_output("D3D9 backend: compiled safe stock-lighting MODEL/Fast/Fogged VS3 shaders.");
            return true;
        }

        static void announce_hit(std::uint32_t bit, const char *name) noexcept {
            if(hit_mask & bit) {
                return;
            }
            hit_mask |= bit;
            console_output("D3D9 safe MODEL VS3: replacing %s.", name);
        }

        static HRESULT STDMETHODCALLTYPE set_vertex_shader_hook(
            IDirect3DDevice9 *device,
            IDirect3DVertexShader9 *shader
        ) {
            if(!original_set_vertex_shader) {
                return D3DERR_INVALIDCALL;
            }

            if(primary_v2_requested()
                && safe_mode
                && !rasterizer_modern_model_test_enabled()
                && vertex_shaders
                && installed_device == device) {
                if(vertex_shaders[VSH_MODEL_FOGGED].shader
                    && shader == vertex_shaders[VSH_MODEL_FOGGED].shader) {
                    announce_hit(1u << 0, "VSH_MODEL_FOGGED");
                    shader = fogged_shader;
                }
                else if(vertex_shaders[VSH_MODEL].shader
                    && shader == vertex_shaders[VSH_MODEL].shader) {
                    announce_hit(1u << 1, "VSH_MODEL");
                    shader = model_shader;
                }
                else if(vertex_shaders[VSH_MODEL_FAST].shader
                    && shader == vertex_shaders[VSH_MODEL_FAST].shader) {
                    announce_hit(1u << 2, "VSH_MODEL_FAST");
                    shader = fast_shader;
                }
            }

            // This hook is intentionally installed after primary_v2. When a
            // safe shader is substituted, primary_v2 sees a non-stock pointer
            // and forwards it unchanged; active-camo/shadow still use primary_v2.
            return original_set_vertex_shader(device, shader);
        }

        static bool install(IDirect3DDevice9 *device) noexcept {
            if(!device || !primary_v2_requested()) {
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
            return true;
        }

        static void on_end_scene(IDirect3DDevice9 *device) noexcept {
            if(primary_v2_requested()) {
                install(device);
            }
        }

        static void set_up() noexcept {
            if(!primary_v2_requested()) {
                return;
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

    inline void set_up_d3d9_model_safe_lighting_vs3() noexcept {
        D3D9ModelSafeLightingVS3::set_up();
    }
}

#endif
